#include "tracker_sync.h"
#include <thread>
#include <arpa/inet.h>
#include <unistd.h>
#include <iostream>
#include <mutex>
#include <cstring>
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
            if((sync_socket=socket(AF_INET, SOCK_STREAM, 0))<0)
            {
                perror("Socket creation failed");
                sleep(2);
                continue;
            }
            struct sockaddr_in addr;
            addr.sin_family=AF_INET;
            addr.sin_port=htons(peer_port);
            inet_pton(AF_INET, peer_ip.c_str(), &addr.sin_addr);

            if(connect(sync_socket, (struct sockaddr*)&addr, sizeof(addr))<0) 
            {
                close(sync_socket);
                sync_socket=-1;
                sleep(2);
                continue;
            }

            char buffer[1024];
            while(1) 
            {
                memset(buffer, 0, sizeof(buffer));
                int bytes=read(sync_socket, buffer, 1024);
                if (bytes<=0) break;

                is_sync_message=true;
                string cmd(buffer, bytes);
                is_sync_message=false;
            }
            close(sync_socket);
            sync_socket=-1;
        }
    }).detach();
}

void send_sync_message(const string &msg) 
{
    lock_guard<mutex> lock(sync_mutex);
    if (sync_socket>0) 
    {
        send(sync_socket, msg.c_str(), msg.size(), 0);
    }
}
