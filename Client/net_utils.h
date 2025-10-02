#ifndef NET_UTILS_H
#define NET_UTILS_H

#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>
#include <string>
#include <arpa/inet.h>
#include <errno.h>

//Send all data in buf of length len through socket sock
inline int send_all(int sock, const char *buf, size_t len)
{
    size_t total=0;
    while(total<len)
    {
        ssize_t sent=send(sock, buf+total, len-total, 0);
        if(sent<=0)
        {
            if(sent<0 && errno==EINTR)
                continue;
            return -1;
        }
        total+=(size_t)sent;
    }
    return 0;
}

//Connect to a peer given its "ip:port" string
inline ssize_t recv_all(int sock, char *buf, size_t len)
{
    size_t total=0;
    while(total<len)
    {
        ssize_t r=recv(sock, buf+total, len-total, 0);
        if(r<=0)
        {
            if(r<0 && errno==EINTR)
                continue;
            return -1;
        }
        total+=(size_t)r;
    }
    return (ssize_t)total;
}

//Function to connect to a tracker given its IP and port
inline bool recv_line(int sock, std::string &out)
{
    out.clear();
    char c;
    while(true)
    {
        ssize_t r=recv(sock, &c, 1, 0);
        if(r<=0)
        {
            if(r<0 && errno==EINTR)
                continue;
            return false;
        }
        if(c=='\n')
            break;
        out.push_back(c);
    }
    return true;
}

#endif
