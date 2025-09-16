#ifndef TRACKER_SYNC_H
#define TRACKER_SYNC_H

#include <string>
#include <thread>
#include <mutex>
using namespace std;

extern int sync_socket;
extern mutex sync_mutex;
extern bool is_sync_message;

void start_sync_thread(string peer_ip, int peer_port);
void send_sync_message(const string &msg);

#endif