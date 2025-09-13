#include <iostream>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>
#include <thread>
#include <fcntl.h>
#include <cstring>
using namespace std;

void client_server(int peer_port)
{
    int client, new_socket;
    struct sockaddr_in client_address;
    socklen_t addrlen=sizeof(client_address);

    if((client=socket(AF_INET, SOCK_STREAM, 0))<0)
    {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);
    }

    int opt=1;
    if(setsockopt(client, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)))
    {
        perror("Set socket options failed");
        exit(EXIT_FAILURE);
    }

    
    client_address.sin_family=AF_INET;
    client_address.sin_addr.s_addr=INADDR_ANY;
    client_address.sin_port=htons(peer_port);

    if(bind(client, (struct sockaddr *)&client_address, sizeof(client_address))<0)
    {
        perror("Bind failed: Port already in use");
        close(client);
        exit(EXIT_FAILURE);
    }

    if(listen(client, 10)<0)
    {
        perror("Listen failed");
        exit(EXIT_FAILURE);
    }

    while(1)
    {
        if((new_socket=accept(client, (struct sockaddr *)&client_address, &addrlen))<0)
        {
            perror("Accept failed");
            exit(EXIT_FAILURE);
        }

        char buffer[1024]={0};
        int bytes=read(new_socket, buffer, 1024);
        if(bytes>0)
        {
            cout<<"Peer request: "<<buffer<<endl;
            string reply="Dummy file data from peer";
            send(new_socket, reply.c_str(), reply.size(), 0);
        }
        close(new_socket);
    }
}

int main(int argc , char *argv[])
{
    if(argc!=3)
    {
        cout<<"Usage: "<<argv[0]<<" <ip:port> tracker_info.txt"<<endl;
        return 0;
    }
    string peer_info=argv[1];
    size_t pos=peer_info.find(':');
    if(pos==(size_t)-1)
    {
        cout<<"Invalid peer info format. Use <ip:port>"<<endl;
        return 0;
    }

    string peer_ip=peer_info.substr(0, pos);
    int peer_port;
    try
    {
        peer_port=stoi(peer_info.substr(pos+1));
        if(peer_port<1024 || peer_port>65535)
        {
            cout<<"Port number must be between 1024 and 65535"<<endl;
            return 0;
        }
    }
    catch(exception& e)
    {
        cout<<e.what()<<endl;
    }
    
    char* filename=argv[2];

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
    
    thread t(client_server, peer_port);
    t.detach();

    string ip;
    int port;
    {
        string line(file_buffer);
        size_t space_pos=line.find(' ');
        if(space_pos==(size_t)-1)
        {
            cout<<"Invalid tracker info format in file. Use <ip> <port>"<<endl;
            return 0;
        }
        ip=line.substr(0, space_pos);
        try
        {
            port=stoi(line.substr(space_pos+1));
            if(port<1024 || port>65535)
            {
                cout<<"Port number must be between 1024 and 65535"<<endl;
                return 0;
            }
        }
        catch(exception& e)
        {
            cout<<e.what()<<endl;
            return 0;
        }
    }

    cout<<"Client peer running at "<<peer_ip<<":"<<peer_port<<endl;
    cout<<"Connecting to tracker at "<<ip<<":"<<port<<endl;


    int sock=0;
    struct sockaddr_in server_address;
    
    if((sock=socket(AF_INET, SOCK_STREAM, 0))<0)
    {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);
    }
    
    server_address.sin_family=AF_INET;
    server_address.sin_port=htons(port);
    
    if(inet_pton(AF_INET, ip.c_str(), &server_address.sin_addr)<=0)
    {
        perror("Invalid address/Address not supported");
        exit(EXIT_FAILURE);
    }
    
    if(connect(sock, (struct sockaddr *)&server_address, sizeof(server_address))<0)
    {
        perror("Connection failed");
        exit(EXIT_FAILURE);
    }
    
    while(1)
    {
        cout<<">> ";
        string command;
        getline(cin, command);
        if(command=="exit" || command=="quit")
        {
            string logout_msg="logout\n";
            send(sock, logout_msg.c_str(), logout_msg.size(), 0);
            break;
        }
        if(command.rfind("login", 0)==0)
        {
            command+=" "+to_string(peer_port);
        }
        string message=command+"\n";
        send(sock, message.c_str(), message.size(), 0);
        char buffer[1024]={0};
        int bytes=read(sock, buffer, 1024);
        if(bytes>0)
        {
            cout<<"Tracker>> "<<buffer<<endl;
        }
        else if(bytes==0)
        {
            cout<<"Connection to tracker lost. Exiting...\n";
            close(sock);
            exit(0);
        }
        else
        {
            perror("Read error");
            close(sock);
            exit(0);
        }
    }
    close(sock);
    return 0;
}    