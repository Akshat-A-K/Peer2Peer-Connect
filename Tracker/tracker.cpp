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
using namespace std;

bool running=true;
mutex run_mutex;

void client_handle(int client)
{
    char buffer[1024]={0};

    while(1)
    {
        memset(buffer, 0, sizeof(buffer));
        int bytes=read(client, buffer, 1024);
        if(bytes>0)
        {
            string command(buffer, bytes);
            string response="";

            while(!command.empty() && (command.back()=='\n' || command.back()=='\r')) 
                command.pop_back();

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
                response=create_user(tokens);
            }
            else if(tokens[0]=="login")
            {
                response=login(tokens, client);
            }
            else if(tokens[0]=="logout")
            {
                response=logout(tokens, client);
            }
            else if(tokens[0]=="create_group")
            {
                response=create_group(tokens, client);
            }
            else if(tokens[0]=="list_groups")
            {
                response=list_groups(tokens, client);
            }
            else if(tokens[0]=="join_group")
            {
                response=join_group(tokens, client);
            }
            else if(tokens[0]=="accept_request")
            {
                response=accept_request(tokens, client);
            }
            else if(tokens[0]=="leave_group")
            {
                response=leave_group(tokens, client);
            }
            else if(tokens[0]=="list_requests")
            {
                response=list_requests(tokens, client);
            }
            else if(tokens[0]=="upload_file")
            {
                // response=upload_file(tokens, client);
            }
            else if(tokens[0]=="download_file")
            {
                // response=download_file(tokens, client);
            }
            else if(tokens[0]=="list_files")
            {
                // response=list_files_in_group(tokens, client);
            }
            else if(tokens[0]=="stop_share")
            {
                // response=stop_share(tokens, client);
            }
            else
            {
                response="Unknown command";
            }
            if(!is_sync_message) 
            {
                if(tokens[0]!="list_groups" && tokens[0]!="list_requests")
                    send_sync_message(command); 
            }
            send(client, response.c_str(), response.size(), 0);
        }
        else
        {
            close(client);
            return;
        }
    }
}

int main(int argc, char* argv[])
{
    if(argc!=3)
    {
        cout<<"Usage: "<<argv[0]<<" tracker_info.txt <tracker_no>"<<endl;
        return 0;
    }
    
    char* filename=argv[1];
    int tracker_no=0;
    try
    {
        tracker_no=stoi(argv[2]);
    }
    catch(exception& e)
    {
        cout<<e.what()<<endl;
    }

    int fd=open(filename, O_RDONLY);
    if(fd<0)
    {
        perror("File open error");
        return 0;
    }

    char file_buffer[1024]={0};
    int bytes=read(fd, file_buffer, 1024);
    close(fd);
    if(bytes<=0)
    {
        perror("File read error");
        return 0;
    }

    file_buffer[bytes]='\0';

    vector<string> lines;
    string line;
    istringstream iss(string(file_buffer, bytes));
    while(getline(iss, line))
    {
        while(!line.empty() && (line.back()=='\n' || line.back()=='\r')) 
            line.pop_back();
        if(!line.empty())
            lines.push_back(line);
    }

    if(tracker_no<1 || tracker_no>(int)lines.size())
    {
        cout<<"Invalid tracker number"<<endl;
        return 0;
    }

    string ip;
    int port;
    {
        istringstream ls(lines[tracker_no-1]);
        ls>>ip>>port;
    }

    string peer_ip;
    int peer_port;
    {
        int peer_no=(tracker_no==1)?2:1;
        istringstream ls(lines[peer_no-1]);
        ls>>peer_ip>>peer_port;
    }

    start_sync_thread(peer_ip, peer_port);
    sleep(1);
    
    cout<<"Tracker "<<tracker_no<<" running at "<<ip<<":"<<port<<endl;

    thread console_thread([]() 
    {
        string cmd;
        while(1) 
        {
            if(!getline(cin, cmd)) 
                break;
            if(cmd=="exit" || cmd=="quit") 
            {
                lock_guard<mutex> lock(run_mutex);
                running=false;
                shutdown(sync_socket, SHUT_RDWR); 
                break;
            }
        }
    });
    console_thread.detach();

    int server;
    struct sockaddr_in server_address;
    int opt=1;

    if((server=socket(AF_INET, SOCK_STREAM, 0))<0) 
    {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);
    }

    if(setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)))
    {
        perror("Set socket options failed");
        exit(EXIT_FAILURE);
    }

    #ifdef SO_REUSEPORT
    if(setsockopt(server, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt))<0) 
    {
        perror("setsockopt REUSEPORT");
        exit(EXIT_FAILURE);
    }
    #endif

    server_address.sin_family=AF_INET;
    if(inet_pton(AF_INET, ip.c_str(), &server_address.sin_addr)<=0) 
    {
        perror("Invalid tracker bind IP");
        exit(EXIT_FAILURE);
    }
    server_address.sin_port=htons(port);

    if(bind(server, (struct sockaddr *)&server_address, sizeof(server_address))<0)
    {
        perror("Bind failed");
        exit(EXIT_FAILURE);
    }

    if(listen(server, 10)<0)
    {
        perror("Listen failed");
        exit(EXIT_FAILURE);
    }

    while(true)
    {
        {
            lock_guard<mutex> lock(run_mutex);
            if(!running) 
                break;
        }

        struct sockaddr_in client;
        socklen_t client_length=sizeof(client);
        int client_fd=accept(server, (struct sockaddr *)&client, &client_length);
        if(client_fd<0)
        {
            lock_guard<mutex> lock(run_mutex);
            if(!running)
                break;
            perror("Accept failed");
            continue;
        }
        thread(client_handle, client_fd).detach();
    }
    close(server);
    cout<<"Tracker shutting down..."<<endl;
    return 0;
}