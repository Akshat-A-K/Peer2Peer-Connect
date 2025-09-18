#include "tracker_sync.h"
#include <thread>
#include <arpa/inet.h>
#include <unistd.h>
#include <iostream>
#include <mutex>
#include <cstring>
#include <sstream>
#include <vector>
#include "user.h"
using namespace std;

int sync_socket=-1;
bool is_sync_message=false;
mutex sync_mutex;

void start_sync_thread(string peer_ip, int peer_port) 
{
    thread([peer_ip, peer_port]() 
    {
        while(1) 
        {
            int s;
            if((s=socket(AF_INET, SOCK_STREAM, 0))<0)
            {
                perror("Socket creation failed");
                sleep(2);
                continue;
            }
            struct sockaddr_in addr;
            addr.sin_family=AF_INET;
            addr.sin_port=htons(peer_port);
            inet_pton(AF_INET, peer_ip.c_str(), &addr.sin_addr);

            if(connect(s, (struct sockaddr*)&addr, sizeof(addr))<0) 
            {
                close(s);
                sync_socket=-1;
                sleep(2);
                continue;
            }

            {
                lock_guard<mutex> lock(sync_mutex);
                sync_socket=s;
            }

            char buffer[1024];
            while(1) 
            {
                memset(buffer, 0, sizeof(buffer));
                int bytes=read(s, buffer, 1024);
                if(bytes<=0) break;

                is_sync_message=true;
                string cmd(buffer, bytes);

                while(!cmd.empty() && (cmd.back()=='\n' || cmd.back()=='\r')) 
                    cmd.pop_back();
                
                vector<string> tokens;
                string token;
                istringstream iss(cmd);
                while(iss>>token)
                {
                    tokens.push_back(token);
                }
                if(tokens.size()==0) continue;
                else
                {
                    if(tokens[0]=="create_user")
                    {
                        create_user(tokens);
                    }
                    else if(tokens[0]=="create_group")
                    {
                        create_group(tokens, -1);
                    }
                    else if(tokens[0]=="join_group")
                    {
                        join_group(tokens, -1);
                    }
                    else if(tokens[0]=="accept_request")
                    {
                        accept_request(tokens, -1);
                    }
                    else if(tokens[0]=="leave_group")
                    {
                        leave_group(tokens, -1);
                    }
                    else if(tokens[0]=="list_groups" || tokens[0]=="list_requests")
                    {
                        
                    }
                }
                is_sync_message=false;
            }
            close(s);
            sync_socket=-1;
            sleep(2);
        }
    }).detach();
}

void send_sync_message(const string &msg) 
{
    lock_guard<mutex> lock(sync_mutex);
    if (sync_socket>0) 
    {
        string m=msg+"\n";
        send(sync_socket, m.c_str(), m.size(), 0);
    }
}
