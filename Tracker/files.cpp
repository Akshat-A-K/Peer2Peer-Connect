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

unordered_map<string, vector<FileInfo>> group_files;
unordered_map<string, FileInfo> all_files;
mutex files_mutex;

long get_filesize(const string &filepath)
{
    struct stat stat_buf;
    int rc = stat(filepath.c_str(), &stat_buf);
    return rc == 0 ? stat_buf.st_size : -1;
}

string compute_sha1(const string &filepath)
{
    int fd = open(filepath.c_str(), O_RDONLY);
    if (fd == -1)
        return "";
    SHA_CTX ctx;
    SHA1_Init(&ctx);
    char buffer[8192];
    ssize_t bytes_read;
    while ((bytes_read = read(fd, buffer, sizeof(buffer))) > 0)
        SHA1_Update(&ctx, buffer, bytes_read);
    if (bytes_read == -1)
    {
        close(fd);
        return "";
    }
    unsigned char hash[20];
    SHA1_Final(hash, &ctx);
    char hexstr[41];
    for (int i = 0; i < 20; i++)
        sprintf(hexstr + i * 2, "%02x", hash[i]);
    close(fd);
    return string(hexstr);
}

vector<string> compute_piece_hashes(const string &filepath, size_t chunk_size)
{
    vector<string> hashes;
    int fd = open(filepath.c_str(), O_RDONLY);
    if (fd == -1)
        return hashes;
    unsigned char *buffer = new unsigned char[chunk_size];
    ssize_t bytes;
    while ((bytes = read(fd, buffer, chunk_size)) > 0)
    {
        unsigned char hash[SHA_DIGEST_LENGTH];
        SHA1(buffer, bytes, hash);
        char hex[SHA_DIGEST_LENGTH * 2 + 1];
        for (int i = 0; i < SHA_DIGEST_LENGTH; i++)
            sprintf(hex + i * 2, "%02x", hash[i]);
        hex[SHA_DIGEST_LENGTH * 2] = 0;
        hashes.emplace_back(hex);
    }
    delete[] buffer;
    close(fd);
    return hashes;
}

string upload_file(const vector<string> &tokens, int client_fd)
{
    if (tokens.size() != 3)
        return "Usage: upload_file <group_id> <file_path>";

    string group_id = tokens[1];
    string filepath = tokens[2];
    string filename = filepath.substr(filepath.find_last_of("/\\") + 1);

    long filesize = get_filesize(filepath);
    if (filesize == -1)
        return "File does not exist on disk";

    string full_hash = compute_sha1(filepath);
    if (full_hash.empty())
        return "Error computing SHA-1";

    vector<string> piece_hashes = compute_piece_hashes(filepath, 512 * 1024);

    lock_guard<mutex> lock(files_mutex);
    if (group_files.find(group_id) == group_files.end())
        return "Group does not exist";

    string peerid = to_string(client_fd);
    if (client_user.count(client_fd))
    {
        string username = client_user[client_fd];
        if (user_ports.count(username))
            peerid = user_ports[username];
        else
            peerid = username;
    }

    // If file already exists in group, add this peer to shared_by
    for (auto &ff : group_files[group_id])
    {
        if (ff.filename == filename)
        {
            ff.shared_by.insert(peerid);
            all_files[ff.filepath] = ff;

            string m = "upload_file " + group_id + " " + filepath + " " + to_string(filesize) + " " + full_hash + " " + to_string(piece_hashes.size());
            for (auto &ph : piece_hashes)
                m += " " + ph;
            m += " SHARED_BY";
            for (auto &s : ff.shared_by)
                m += " " + s;

            cout << "Added peer " << peerid << " for " << filename << endl;
            if (!is_sync_message)
                send_sync_message(m);

            return "Added you as a peer for: " + filename;
        }
    }

    // New file
    FileInfo f;
    f.filename = filename;
    f.filepath = filepath;
    f.group_id = group_id;
    f.filesize = filesize;
    f.sha1_full = full_hash;
    f.piece_hashes = piece_hashes;
    f.shared_by.insert(peerid);

    group_files[group_id].push_back(f);
    all_files[filepath] = f;

    string m = "upload_file " + group_id + " " + filepath + " " + to_string(filesize) + " " + full_hash + " " + to_string(piece_hashes.size());
    for (auto &ph : piece_hashes)
        m += " " + ph;
    m += " SHARED_BY";
    for (auto &s : f.shared_by)
        m += " " + s;

    cout << "File uploaded: " << filename << " by peer " << peerid << endl;
    if (!is_sync_message)
        send_sync_message(m);

    return "File uploaded: " + filename;
}

string list_files_in_group(const vector<string> &tokens, int client_fd)
{
    if (tokens.size() != 2)
        return "Usage: list_files <group_id>";
    string group_id = tokens[1];
    lock_guard<mutex> lock(files_mutex);
    if (group_files.find(group_id) == group_files.end())
        return "No files in group " + group_id;
    string response = "Files in group " + group_id + ":\n";
    if (!group_files[group_id].empty())
    {
        for (auto &f : group_files[group_id])
            response += f.filename + " (" + to_string(f.filesize) + " bytes)\n";
    }
    else
        response = "No files in group " + group_id;
    return response;
}

string download_file(const vector<string> &tokens, int client_fd)
{
    if (tokens.size() != 4)
        return "Usage: download_file <group_id> <file_name> <destination_path>";

    string group_id = tokens[1];
    string filename = tokens[2];

    lock_guard<mutex> lock(files_mutex);

    // Check if group exists
    if (group_files.find(group_id) == group_files.end())
        return "ERROR No such group";

    // Find the file in the group
    for (auto &f : group_files[group_id])
    {
        if (f.filename == filename)
        {
            // Verify that each peer actually has the full file
            set<string> valid_peers;
            for (const auto &p : f.shared_by)
            {
                valid_peers.insert(p);
            }

            if (valid_peers.empty())
                return "ERROR No peers with full file available";

            // Build the response
            string resp = "FOUND " + to_string(f.filesize) + " " + f.sha1_full + " " +
                          to_string((int)f.piece_hashes.size()) + " " + f.filepath + "\n";

            for (size_t i = 0; i < f.piece_hashes.size(); i++)
            {
                resp += f.piece_hashes[i];
                if (i + 1 < f.piece_hashes.size())
                    resp += " ";
            }
            resp += "\nPEERS ";

            bool first = true;
            for (const auto &p : valid_peers)
            {
                if (!first)
                    resp += " ";
                resp += p;
                first = false;
            }
            resp += "\n";
            return resp;
        }
    }

    return "ERROR File not found in group";
}

string stop_share(const vector<string> &tokens, int client_fd)
{
    if (tokens.size() != 3)
        return "Usage: stop_share <group_id> <file_name>";

    string group_id = tokens[1];
    string filename = tokens[2];
    lock_guard<mutex> lock(files_mutex);

    string peerid = to_string(client_fd);

    if (group_files.find(group_id) == group_files.end())
        return "No such group";

    if (client_user.count(client_fd))
    {
        string username = client_user[client_fd];
        if (user_ports.count(username))
            peerid = user_ports[username];
        else
            peerid = username;
    }

    for (auto it = all_files.begin(); it != all_files.end(); ++it)
    {
        if (it->second.filename == filename && it->second.group_id == group_id)
        {
            FileInfo f = it->second;

            f.shared_by.erase(peerid);

            if (f.shared_by.empty())
            {
                auto &vec = group_files[f.group_id];
                vec.erase(remove_if(vec.begin(), vec.end(),
                                    [&](const FileInfo &fi)
                                    { return fi.filename == filename; }),
                          vec.end());
                string path_key = it->first;
                all_files.erase(it);
                cout << "Stopped sharing and removed file " << filename << " from group " << group_id << endl;
            }
            else
            {
                all_files[it->first].shared_by = f.shared_by;
                for (auto &gf : group_files[f.group_id])
                {
                    if (gf.filename == filename)
                        gf.shared_by = f.shared_by;
                }
                cout << "Removed peer " << peerid << " from file " << filename << " in group " << group_id << endl;
            }
            return "Stopped sharing file: " + filename;
        }
    }
    return "File not found";
}
