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
#include "files.h"
#include <algorithm>
using namespace std;

// Global variables for sync thread
int sync_socket = -1;
bool is_sync_message = false;
mutex sync_mutex; // protects sync_socket

// Function to start the sync thread
void start_sync_thread(string peer_ip, int peer_port)
{
    // Start a detached thread that maintains a persistent connection to the peer
    auto worker = [=]() {
        while (1)
        {
            int s = socket(AF_INET, SOCK_STREAM, 0);
            if (s < 0)
            {
                perror("Socket creation failed");
                sleep(2);
                continue;
            }

            struct sockaddr_in addr;
            addr.sin_family = AF_INET;
            addr.sin_port = htons(peer_port);
            inet_pton(AF_INET, peer_ip.c_str(), &addr.sin_addr);

            if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) < 0)
            {
                close(s);
                {
                    lock_guard<mutex> lock(sync_mutex);
                    sync_socket = -1;
                }
                sleep(2);
                continue;
            }

            {
                lock_guard<mutex> lock(sync_mutex);
                sync_socket = s;
            }

            string recv_buf;
            char buffer[1024];
            while (1)
            {
                memset(buffer, 0, sizeof(buffer));
                int bytes = read(s, buffer, sizeof(buffer));
                if (bytes <= 0)
                    break;

                recv_buf.append(buffer, bytes);

                size_t pos;
                while ((pos = recv_buf.find('\n')) != string::npos)
                {
                    string cmd = recv_buf.substr(0, pos);
                    recv_buf.erase(0, pos + 1);

                    while (!cmd.empty() && (cmd.back() == '\r'))
                        cmd.pop_back();

                    is_sync_message = true;
                    vector<string> tokens;
                    string token;
                    istringstream iss(cmd);
                    while (iss >> token)
                        tokens.push_back(token);

                    if (tokens.empty())
                    {
                        is_sync_message = false;
                        continue;
                    }

                    // Handle commands (same logic as before)
                    if (tokens[0] == "create_user")
                        create_user(tokens);
                    else if (tokens[0] == "create_group")
                        create_group(tokens, -1);
                    else if (tokens[0] == "join_group")
                        join_group(tokens, -1);
                    else if (tokens[0] == "accept_request")
                        accept_request(tokens, -1);
                    else if (tokens[0] == "leave_group")
                        leave_group(tokens, -1);
                    else if (tokens[0] == "upload_file")
                    {
                        if (tokens.size() < 6)
                        {
                            // incomplete message
                        }
                        else
                        {
                            try
                            {
                                string group_id = tokens[1];
                                string filepath = tokens[2];
                                long filesize = stol(tokens[3]);
                                string sha1_full = tokens[4];
                                int num_pieces = stoi(tokens[5]);

                                vector<string> piece_hashes;
                                int idx = 6;
                                for (int i = 0; i < num_pieces && idx < (int)tokens.size(); ++i, ++idx)
                                    piece_hashes.push_back(tokens[idx]);

                                unordered_set<string> shared_by;
                                if (idx < (int)tokens.size() && tokens[idx] == "SHARED_BY")
                                {
                                    ++idx;
                                    while (idx < (int)tokens.size())
                                        shared_by.insert(tokens[idx++]);
                                }

                                FileInfo f;
                                f.filepath = filepath;
                                f.filename = filepath.substr(filepath.find_last_of("/\\") + 1);
                                f.group_id = group_id;
                                f.filesize = filesize;
                                f.sha1_full = sha1_full;
                                f.piece_hashes = piece_hashes;
                                f.shared_by = shared_by;

                                lock_guard<mutex> lock(files_mutex);
                                group_files[group_id].push_back(f);
                                all_files[filepath] = f;
                            }
                            catch (const exception &e)
                            {
                                cerr << "Error processing upload_file sync message: " << e.what() << " -- skipping\n";
                            }
                        }
                    }
                    else if (tokens[0] == "stop_share")
                    {
                        if (tokens.size() >= 4)
                        {
                            string group_id = tokens[1];
                            string filename = tokens[2];
                            string peerid = tokens[3];

                            lock_guard<mutex> lock(files_mutex);
                            if (group_files.count(group_id))
                            {
                                for (auto &gf : group_files[group_id])
                                    if (gf.filename == filename)
                                        gf.shared_by.erase(peerid);

                                for (auto it = all_files.begin(); it != all_files.end();)
                                {
                                    if (it->second.filename == filename && it->second.group_id == group_id)
                                    {
                                        if (it->second.shared_by.empty())
                                            it = all_files.erase(it);
                                        else
                                            ++it;
                                    }
                                    else
                                        ++it;
                                }

                                auto &vec = group_files[group_id];
                                vec.erase(remove_if(vec.begin(), vec.end(), [&](const FileInfo &fi) {
                                    return fi.filename == filename && fi.shared_by.empty();
                                }),
                                          vec.end());
                            }
                        }
                    }
                    else if (tokens[0] == "login")
                    {
                        if (tokens.size() >= 4)
                            login(tokens, -1);
                    }
                    else if (tokens[0] == "logout")
                    {
                        if (tokens.size() >= 2)
                            logout_by_username(tokens[1]);
                        else
                            logout(tokens, -1);
                    }
                    // list_groups/list_requests ignored for sync

                    is_sync_message = false;
                }
            }

            close(s);
            {
                lock_guard<mutex> lock(sync_mutex);
                sync_socket = -1;
            }
            sleep(2);
        }
    };

    std::thread t(worker);
    t.detach();
}

// Function to send a sync message to the connected peer
void send_sync_message(const string &msg)
{
    lock_guard<mutex> lock(sync_mutex);// protect sync_socket
    // Send only if connected and not a sync message itself
    if(sync_socket>0)
    {
        string m=msg+"\n";
        send(sync_socket, m.c_str(), m.size(), 0);
    }
}
