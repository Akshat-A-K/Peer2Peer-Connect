#include "files.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <openssl/sha.h>
#include <cstring>
#include <iostream>
#include <vector>
#include <algorithm>
#include "net_utils.h"
#include "user.h"
#include "tracker_sync.h"

unordered_map<string, vector<FileInfo>> group_files; //group id -> list of files
unordered_map<string, FileInfo> all_files;//filepath -> FileInfo
mutex files_mutex;//protects group_files and all_files

//Get the size of a file
long get_filesize(const string &filepath)
{
    struct stat stat_buf;
    int rc=stat(filepath.c_str(), &stat_buf);
    return rc==0 ? stat_buf.st_size : -1;
}

//Compute SHA-1 hash of a file
string compute_sha1(const string &filepath)
{
    int fd=open(filepath.c_str(), O_RDONLY);
    if(fd==-1)
        return "";
    SHA_CTX ctx;
    SHA1_Init(&ctx);
    char buffer[8192];
    ssize_t bytes_read;
    while((bytes_read=read(fd, buffer, sizeof(buffer)))>0) //Read file in chunks
        SHA1_Update(&ctx, buffer, bytes_read); //Update SHA-1 hash
    if(bytes_read==-1)
    {
        close(fd);
        return "";
    }
    unsigned char hash[20];
    SHA1_Final(hash, &ctx);
    char hexstr[41];
    for(int i=0; i < 20; i++)
        sprintf(hexstr+i * 2, "%02x", hash[i]);
    close(fd);
    return string(hexstr);
}

//Compute SHA-1 hashes of file pieces
vector<string> compute_piece_hashes(const string &filepath, size_t chunk_size)
{
    vector<string> hashes;//Vector for piece hashes

    //Open file for reading
    int fd=open(filepath.c_str(), O_RDONLY);
    if(fd==-1)
        return hashes;
    
    //Read file in chunks and compute SHA-1 for each piece
    unsigned char *buffer=new unsigned char[chunk_size];
    ssize_t bytes;

    while((bytes=read(fd, buffer, chunk_size))>0)
    {
        //Compute SHA-1 of the piece
        unsigned char hash[SHA_DIGEST_LENGTH];
        SHA1(buffer, bytes, hash);
        char hex[SHA_DIGEST_LENGTH * 2+1];
        for(int i=0; i<SHA_DIGEST_LENGTH; i++)
            sprintf(hex+i*2, "%02x", hash[i]);
        hex[SHA_DIGEST_LENGTH*2]=0;
        hashes.emplace_back(hex); //Store piece hash
    }
    delete[] buffer;
    close(fd);
    return hashes;
}

//Function to handle upload_file command for tracker
string upload_file(const vector<string> &tokens, int client_fd)
{
    if(tokens.size()!=3)
        return "Usage: upload_file <group_id> <file_path>";

    string group_id=tokens[1]; //group id
    string filepath=tokens[2]; //file path
    //Extract filename from filepath
    string filename=filepath.substr(filepath.find_last_of("/\\")+1);

    long filesize=get_filesize(filepath);
    if(filesize==-1)
        return "File does not exist on disk";

    
    //Compute full file SHA-1
    string full_hash=compute_sha1(filepath);
    if(full_hash.empty())
        return "Error computing SHA-1";

    //Compute piece hashes (512KB pieces)
    vector<string> piece_hashes=compute_piece_hashes(filepath, 512 * 1024);

    lock_guard<mutex> lock(files_mutex);
    if(group_files.find(group_id)==group_files.end()) //group does not exist
        return "Group does not exist";

    //Identify peer
    string peerid=to_string(client_fd);

    //If user logged in, use username or port as peer id
    if(client_user.count(client_fd))
    {
        string username=client_user[client_fd]; //username of client
        if(user_ports.count(username))//if user has registered a port, use that as peer id
            peerid=user_ports[username];
        else
            peerid=username;
    }

    //If file already exists in group, add this peer to shared_by
    for(auto &ff : group_files[group_id])
    {
        if(ff.filename==filename)
        {
            //File already exists, just add this peer to shared_by
            ff.shared_by.insert(peerid);
            all_files[ff.filepath]=ff;

            //Send sync message about updated shared_by
            string m="upload_file "+group_id+" "+filepath+" "+to_string(filesize)+" "+full_hash+" "+to_string(piece_hashes.size());
            for(auto &ph : piece_hashes)
                m+=" "+ph;
            
            // Append updated SHARED_BY list
            m+=" SHARED_BY";
            for(auto &s : ff.shared_by)
                m+=" "+s;

            //Log the addition
            cout<<"Added peer "<<peerid<<" for "<<filename<<endl;
            if(!is_sync_message)
                send_sync_message(m);

            return "Added you as a peer for: "+filename;
        }
    }

    // New file details
    FileInfo f;
    f.filename=filename;
    f.filepath=filepath;
    f.group_id=group_id;
    f.filesize=filesize;
    f.sha1_full=full_hash;
    f.piece_hashes=piece_hashes;
    f.shared_by.insert(peerid);

    group_files[group_id].push_back(f);
    all_files[filepath]=f;

    //Send sync message about new file
    string m="upload_file "+group_id+" "+filepath+" "+to_string(filesize)+" "+full_hash+" "+to_string(piece_hashes.size());
    for(auto &ph : piece_hashes)
        m+=" "+ph;
    // Append SHARED_BY list
    m+=" SHARED_BY";
    for(auto &s : f.shared_by)
        m+=" "+s;

    //Log the upload
    cout<<"File uploaded: "<<filename<<" by peer "<<peerid<<endl;
    if(!is_sync_message)
        send_sync_message(m);

    return "File uploaded: "+filename;
}

//Function to handle list_files command for tracker
string list_files_in_group(const vector<string> &tokens, int client_fd)
{
    if(tokens.size()!=2)
        return "Usage: list_files <group_id>";
    
    //List files in the specified group
    string group_id=tokens[1];
    lock_guard<mutex> lock(files_mutex);
    if(group_files.find(group_id)==group_files.end())
        return "No files in group "+group_id;
    string response="Files in group "+group_id+":\n";
    
    //List all files in the group
    if(!group_files[group_id].empty())
    {
        for(auto &f : group_files[group_id])
            response+=f.filename+" ("+to_string(f.filesize)+" bytes)\n";
    }
    else
        response="No files in group "+group_id;
    return response;
}

//Function to handle download_file command for tracker
string download_file(const vector<string> &tokens, int client_fd)
{
    if(tokens.size()!=4)
        return "Usage: download_file <group_id> <file_name> <destination_path>";

    //Extract parameters
    string group_id=tokens[1];
    string filename=tokens[2];

    lock_guard<mutex> lock(files_mutex);

    //Check if group exists
    if(group_files.find(group_id)==group_files.end())
        return "ERROR No such group";

    //Find the file in the group
    for(auto &f : group_files[group_id])
    {
        if(f.filename==filename)
        {
            //Verify that each peer actually has the full file
            set<string> valid_peers;
            for(const auto &p : f.shared_by)
            {
                valid_peers.insert(p);
            }

            if(valid_peers.empty())
                return "ERROR No peers with full file available";

            //Build the response
            string resp="FOUND "+to_string(f.filesize)+" "+f.sha1_full+" " +to_string((int)f.piece_hashes.size())+" "+f.filepath+"\n";

            //Append piece hashes
            for(size_t i=0; i<f.piece_hashes.size(); i++)
            {
                resp+=f.piece_hashes[i];
                if(i+1<f.piece_hashes.size())
                    resp+=" ";
            }
        
            //Append peer list
            resp+="\nPEERS ";
            bool first=true;
            for(const auto &p : valid_peers)
            {
                if(!first)
                    resp+=" ";
                resp+=p;
                first=false;
            }
            resp += "\n";
            return resp;
        }
    }

    return "ERROR File not found in group";
}

//Function to handle stop_share command for tracker
string stop_share(const vector<string> &tokens, int client_fd)
{
    if(tokens.size()!=3)
        return "Usage: stop_share <group_id> <file_name>";

    //Extract parameters
    string group_id=tokens[1];
    string filename=tokens[2];
    lock_guard<mutex> lock(files_mutex);

    //Identify peer
    string peerid=to_string(client_fd);

    //If user logged in, use username or port as peer id
    if(group_files.find(group_id)==group_files.end())
        return "No such group";

    //If user logged in, use username or port as peer id
    if(client_user.count(client_fd))
    {
        string username=client_user[client_fd];
        if(user_ports.count(username))//if user has registered a port, use that as peer id
            peerid=user_ports[username];
        else
            peerid=username;
    }

    //Find the file in the group
    for(auto it=all_files.begin(); it != all_files.end(); ++it)
    {
        if(it->second.filename==filename && it->second.group_id==group_id) //if file found
        {
            FileInfo f=it->second;

            //Remove this peer from shared_by
            f.shared_by.erase(peerid);

            //If no peers left sharing, remove file entirely
            if(f.shared_by.empty())
            {
                auto &vec=group_files[f.group_id];
                vec.erase(remove_if(vec.begin(), vec.end(), [&](const FileInfo &fi) {
                    return fi.filename==filename;
                }), vec.end());
                string path_key=it->first;
                all_files.erase(it);
                cout<<"Stopped sharing and removed file "<<filename<<" from group "<<group_id<<endl;
            }
            else
            {
                //Update shared_by in both all_files and group_files
                all_files[it->first].shared_by=f.shared_by;
                for(auto &gf : group_files[f.group_id])
                {
                    if(gf.filename==filename)
                        gf.shared_by=f.shared_by;
                }
                cout<<"Removed peer "<<peerid<<" from file "<<filename<<" in group "<<group_id<<endl;
            }
            return "Stopped sharing file: "+filename;
        }
    }
    return "File not found";
}
