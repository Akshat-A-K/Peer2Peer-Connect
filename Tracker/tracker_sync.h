#ifndef TRACKER_SYNC_H
#define TRACKER_SYNC_H

#include <string>
#include <thread>
#include <mutex>
using namespace std;

extern int sync_socket; //Socket for sync connection
extern mutex sync_mutex;// protects sync_socket
extern bool is_sync_message;// true if processing a sync message

void start_sync_thread(string peer_ip, int peer_port);// Function to start the sync thread
void send_sync_message(const string &msg);// Function to send a sync message to the connected peer

#endif