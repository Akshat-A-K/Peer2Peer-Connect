#include <iostream>
#include <unistd.h>
#include <cstring>
#include <arpa/inet.h>
#include <thread>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include "user.h"
#include <sstream>
#include <fcntl.h>
#include <cstring>
#include <algorithm>
#include "tracker_sync.h"
#include <mutex>
#include "files.h"
using namespace std;

//Global variables for sync thread
bool running=true;
mutex run_mutex;//protects running

//Mutex to protect user data structures
void client_handle(int client)
{
    //Buffer for incoming data
    char buffer[1024]={0};

    while(1)
    {
        //Read command from client
        memset(buffer, 0, sizeof(buffer));
        int bytes=read(client, buffer, 1024);
        if(bytes>0)
        {
            //Process command
            string command(buffer, bytes);
            {
                istringstream _iss(command);
                string _first;
                if(_iss>>_first)
                    cout<<"Received command: "<<_first<<endl;
            }
            string response="";

            //Trim newline characters
            while(!command.empty() && (command.back()=='\n' || command.back()=='\r'))
                command.pop_back();

            //Tokenize command
            vector<string> tokens;
            string token;
            istringstream iss(command);
            while(iss>>token)
            {
                tokens.push_back(token);
            }

            if(tokens.size()==0)
            {
                response="Invalid command";
            }
            else if(tokens[0]=="create_user")
            {
                response=create_user(tokens); // create_user <username> <password> <port
            }
            else if(tokens[0]=="login")
            {
                response=login(tokens, client); // login <username> <password> <port
            }
            else if(tokens[0]=="logout")
            {
                response=logout(tokens, client);// logout <username>
            }
            else if(tokens[0]=="create_group")
            {
                response=create_group(tokens, client);// create_group <group_id>
            }
            else if(tokens[0]=="list_groups")
            {
                response=list_groups(tokens, client);// list_groups
            }
            else if(tokens[0]=="join_group")
            {
                response=join_group(tokens, client);// join_group <group_id>
            }
            else if(tokens[0]=="accept_request")
            {
                response=accept_request(tokens, client);// accept_request <group_id> <username>
            }
            else if(tokens[0]=="leave_group")
            {
                response=leave_group(tokens, client);// leave_group <group_id>
            }
            else if(tokens[0]=="list_requests")
            {
                response=list_requests(tokens, client);// list_requests <group_id>
            }
            else if(tokens[0]=="upload_file")
            {
                response=upload_file(tokens, client);// upload_file <group_id> <file_path>
            }
            else if(tokens[0]=="download_file")
            {
                response=download_file(tokens, client);// download_file <group_id> <file_name> <destination_path>
            }
            else if(tokens[0]=="list_files")
            {
                response=list_files_in_group(tokens, client);// list_files <group_id>
            }
            else if(tokens[0]=="stop_share")
            {
                response=stop_share(tokens, client);// stop_share <group_id> <file_name>
            }
            else
            {
                response="Unknown command";
            }
            //Send sync message if not a sync message itself
            if(!is_sync_message)
            {
                // Avoid sending sync messages for commands that do not change state
                if(tokens[0] != "list_groups" && tokens[0] != "list_requests" && tokens[0] != "list_files")
                    send_sync_message(command);
            }
            //Send response to client
            send(client, response.c_str(), response.size(), 0);
        }
        else
        {
            // client disconnected unexpectedly: perform logout for this client_fd
            try
            {
                // find username for this client_fd
                string uname;
                {
                    lock_guard<mutex> ul(user_mutex);
                    if(client_user.find(client) != client_user.end())
                        uname=client_user[client];
                }
                // if logged in, perform logout
                if(!uname.empty())
                {
                    vector<string> ltok;
                    ltok.push_back("logout");
                    // call logout to remove client mapping
                    string lresp=logout(ltok, client);
                    if(!lresp.empty())
                        cout<<lresp<<endl;
                    // forward logout with username to peer trackers so state is consistent
                    if(!is_sync_message)
                        send_sync_message(string("logout ")+uname);
                }
            }
            catch (...)
            {
                // best-effort: ignore any exception
            }
            close(client);
            return;
        }
    }
}

int main(int argc, char *argv[])
{
    if(argc!=3)
    {
        cout<<"Usage: "<<argv[0]<<" tracker_info.txt <tracker_no>"<<endl;
        return 0;
    }

    char *filename=argv[1];
    
    //Read tracker info from file
    int tracker_no=0;
    try
    {
        tracker_no=stoi(argv[2]);
    }
    catch (exception &e)
    {
        cout<<e.what()<<endl;
    }

    //Ensure valid tracker number
    int fd=open(filename, O_RDONLY);
    if(fd < 0)
    {
        perror("File open error");
        return 0;
    }

    //Read file content
    char file_buffer[1024]={0};
    int bytes=read(fd, file_buffer, 1024);
    close(fd);
    if(bytes<=0)
    {
        perror("File read error");
        return 0;
    }

    file_buffer[bytes]='\0';

    //Parse lines
    vector<string> lines;
    string line;
    istringstream iss(string(file_buffer, bytes));
    while(getline(iss, line))
    {
        while(!line.empty() && (line.back()=='\n' || line.back()=='\r')) //Trim newline characters
            line.pop_back();
        if(!line.empty())
            lines.push_back(line);
    }

    //Validate tracker number
    if(tracker_no<1 || tracker_no>(int)lines.size())
    {
        cout<<"Invalid tracker number"<<endl;
        return 0;
    }

    //Extract this tracker's IP and port
    string ip;
    int port;
    {
        istringstream ls(lines[tracker_no - 1]);
        ls>>ip>>port;
    }

    string peer_ip;
    int peer_port;
    {
        int peer_no=(tracker_no==1) ? 2:1;
        istringstream ls(lines[peer_no-1]);
        ls>>peer_ip>>peer_port;
    }

    //Start sync thread to connect to peer tracker
    start_sync_thread(peer_ip, peer_port);
    sleep(1);

    cout<<"Tracker "<<tracker_no<<" running at "<<ip<<":"<<port<<endl;
    cout<<"Syncing with peer tracker at "<<peer_ip<<":"<<peer_port<<endl;

    //Thread to monitor console input for "exit" command
    thread console_thread([]()
    {
        string cmd;
        while(1)
        {
            if(!getline(cin,cmd))
                break;
            if(cmd=="exit"||cmd=="quit")
            {
                // Stop the sync thread
                lock_guard<mutex> lock(run_mutex);
                running=false;
                shutdown(sync_socket,SHUT_RDWR);
                break;
            }
        } 
    });
    console_thread.detach();//Detach to allow independent execution

    //Set up server socket
    int server;
    struct sockaddr_in server_address;
    int opt=1;

    //Create socket
    if((server=socket(AF_INET, SOCK_STREAM, 0))<0)
    {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);
    }

    //Set socket options to allow re-binding to the same port immediately after program exit
    if(setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)))
    {
        perror("Set socket options failed");
        exit(EXIT_FAILURE);
    }

    // Set SO_REUSEPORT if available (not on macOS)
    #ifdef SO_REUSEPORT
        if(setsockopt(server, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt))<0)
        {
            perror("setsockopt REUSEPORT");
            exit(EXIT_FAILURE);
        }
    #endif

    //Bind socket to specified IP and port
    server_address.sin_family=AF_INET;//IPv4

    //Convert IP address
    if(inet_pton(AF_INET, ip.c_str(), &server_address.sin_addr)<=0)
    {
        perror("Invalid tracker bind IP");
        exit(EXIT_FAILURE);
    }

    //Convert port number
    server_address.sin_port=htons(port);

    //Bind the socket
    if(bind(server, (struct sockaddr *)&server_address, sizeof(server_address))<0)
    {
        perror("Bind failed");
        exit(EXIT_FAILURE);
    }

    //Listen for incoming connections
    if(listen(server, 10) < 0)
    {
        perror("Listen failed");
        exit(EXIT_FAILURE);
    }

    //Accept and handle incoming connections
    while(true)
    {
        //Check if we should keep running
        {
            lock_guard<mutex> lock(run_mutex);
            if(!running)
                break;
        }
        //Accept incoming connection
        {
            lock_guard<mutex> lock(run_mutex);
            if(!running)
                break;
        }

        //Accept incoming connection
        struct sockaddr_in client;
        socklen_t client_length=sizeof(client);
        int client_fd=accept(server, (struct sockaddr *)&client, &client_length);// accept connection
        if(client_fd<0)
        {
            perror("Accept failed");
            continue;
        }
        //Get client IP address
        char cli_ip[INET_ADDRSTRLEN];//Buffer for IP string
        inet_ntop(AF_INET, &client.sin_addr, cli_ip, INET_ADDRSTRLEN);//Convert IP to string
        thread(client_handle, client_fd).detach();//Handle client in detached thread
    }
    close(server);
    cout<<"Tracker shutting down."<<endl;
    return 0;
}