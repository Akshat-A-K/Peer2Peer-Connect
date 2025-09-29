#include <iostream>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>
#include <thread>
#include <fcntl.h>
#include <cstring>
#include <vector>
#include <sstream>
#include "net_utils.h"
#include <atomic>
#include <map>
#include <thread>
#include <condition_variable>
#include <sys/stat.h>
#include <openssl/sha.h>
#include <queue>
#include <iomanip>
using namespace std;

struct DownloadTask
{
    string group_id;
    string filename;
    string dest_path;
    long filesize;
    string sha1_full;
    vector<string> piece_hashes;
    vector<int> piece_status;
    atomic<int> pieces_done;
    int num_pieces;
    mutex m;
    bool completed = false;
    atomic<bool> cancel{false};
};

map<string, shared_ptr<DownloadTask>> active_downloads;
mutex active_downloads_mutex;

long get_filesize(const string &filepath)
{
    struct stat stat_buf;
    int rc = stat(filepath.c_str(), &stat_buf);
    return rc == 0 ? stat_buf.st_size : -1;
}

int connect_to_tracker(const vector<pair<string, int>> &trackers)
{
    int sock = 0;
    struct sockaddr_in server_address;

    for (auto &t : trackers)
    {
        string ip = t.first;
        int port = t.second;

        if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0)
        {
            perror("Socket creation failed");
            exit(EXIT_FAILURE);
        }

        server_address.sin_family = AF_INET;
        server_address.sin_port = htons(port);

        if (inet_pton(AF_INET, ip.c_str(), &server_address.sin_addr) <= 0)
        {
            perror("Invalid address/Address not supported");
            close(sock);
            continue;
        }

        if (connect(sock, (struct sockaddr *)&server_address, sizeof(server_address)) < 0)
        {
            perror("Connection to tracker failed");
            close(sock);
            continue;
        }
        cout << "Connected to tracker at " << ip << ":" << port << endl;
        return sock;
    }
    cout << "Could not connect to any tracker. Exiting..." << endl;
    exit(0);
}

void client_server(int peer_port)
{
    int client, new_socket;
    struct sockaddr_in client_address;
    socklen_t addrlen = sizeof(client_address);

    if ((client = socket(AF_INET, SOCK_STREAM, 0)) < 0)
    {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);
    }

    int opt = 1;
    if (setsockopt(client, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)))
    {
        perror("Set socket options failed");
        exit(EXIT_FAILURE);
    }

    client_address.sin_family = AF_INET;
    client_address.sin_addr.s_addr = INADDR_ANY;
    client_address.sin_port = htons(peer_port);

    if (bind(client, (struct sockaddr *)&client_address, sizeof(client_address)) < 0)
    {
        perror("Bind failed: Port already in use");
        close(client);
        exit(EXIT_FAILURE);
    }

    if (listen(client, 10) < 0)
    {
        perror("Listen failed");
        exit(EXIT_FAILURE);
    }

    while (1)
    {
        if ((new_socket = accept(client, (struct sockaddr *)&client_address, &addrlen)) < 0)
        {
            perror("Accept failed");
            exit(EXIT_FAILURE);
        }

        char buffer[1024] = {0};
        int bytes = read(new_socket, buffer, 1024);
        if (bytes > 0)
        {
            string req(buffer, bytes);
            istringstream iss(req);
            string cmd;
            iss >> cmd;
            if (cmd == "get_piece")
            {
                string filepath;
                int piece_idx;
                if (!(iss >> filepath >> piece_idx))
                {
                    string err = "ERROR invalid get_piece format\n";
                    send_all(new_socket, err.c_str(), err.size());
                }
                else
                {
                    long filesize = get_filesize(filepath);
                    if (filesize < 0)
                    {
                        string err = "ERROR file_not_found\n";
                        send_all(new_socket, err.c_str(), err.size());
                    }
                    else
                    {
                        size_t chunk = 512 * 1024;
                        off_t offset = (off_t)piece_idx * chunk;
                        size_t to_read = chunk;

                        // last piece can be smaller
                        if (offset + (off_t)to_read > filesize)
                            to_read = filesize - offset;

                        int fd = open(filepath.c_str(), O_RDONLY);
                        if (fd < 0)
                        {
                            string err = "ERROR file_open\n";
                            send_all(new_socket, err.c_str(), err.size());
                        }
                        else
                        {
                            lseek(fd, offset, SEEK_SET);
                            char *buf = new char[to_read];
                            ssize_t r = read(fd, buf, to_read);
                            close(fd);

                            if (r != (ssize_t)to_read)
                            {
                                delete[] buf;
                                string err = "ERROR read_fail\n";
                                send_all(new_socket, err.c_str(), err.size());
                            }
                            else
                            {
                                string header = "DATA " + to_string(r) + "\n";
                                send_all(new_socket, header.c_str(), header.size());
                                send_all(new_socket, buf, r);
                                delete[] buf;
                            }
                        }
                    }
                }
            }
            else
            {
                // previous behavior or error
                string reply = "ERROR unknown_command\n";
                send_all(new_socket, reply.c_str(), reply.size());
            }
        }
        close(new_socket);
    }
}

void upload_file(int tracker_fd, const string &group_id, const string &filepath)
{
    string msg = "upload_file " + group_id + " " + filepath + "\n";
    send(tracker_fd, msg.c_str(), msg.size(), 0);

    char buffer[1024] = {0};
    int bytes = read(tracker_fd, buffer, sizeof(buffer));
    if (bytes > 0)
        cout << "Tracker>> " << string(buffer, bytes) << endl;
}

static string sha1_buf(const unsigned char *buf, size_t len)
{
    unsigned char hash[SHA_DIGEST_LENGTH];
    SHA1(buf, len, hash);
    char hex[SHA_DIGEST_LENGTH * 2 + 1];
    for (int i = 0; i < SHA_DIGEST_LENGTH; i++)
        sprintf(hex + i * 2, "%02x", hash[i]);
    hex[SHA_DIGEST_LENGTH * 2] = 0;
    return string(hex);
}

int connect_to_peer(const string &peer)
{
    size_t p = peer.find(':');
    if (p == string::npos)
        return -1;
    string ip = peer.substr(0, p);
    int port = stoi(peer.substr(p + 1));
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0)
        return -1;
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) <= 0)
    {
        close(s);
        return -1;
    }
    if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        close(s);
        return -1;
    }
    return s;
}

void download_file(int tracker_fd, const string &args)
{
    istringstream iss(args);
    string group, filename, dest_path;
    if (!(iss >> group >> filename >> dest_path))
    {
        cout << "Usage: download_file <group_id> <file_name> <destination_path>\n";
        return;
    }

    // Request tracker for file info
    string msg = "download_file " + group + " " + filename + " " + dest_path + "\n";
    send(tracker_fd, msg.c_str(), msg.size(), 0);

    string resp;
    char buf[8192];
    while (1)
    {
        int n = read(tracker_fd, buf, sizeof(buf));
        if (n <= 0)
        {
            cout << "Tracker read error\n";
            return;
        }
        resp.append(buf, n);

        size_t pos = resp.find("PEERS");
        if (pos != (size_t)-1 && resp.find('\n', pos) != (size_t)-1)
            break;

        if (resp.size() > 10 * 1024 * 1024)
        {
            cout << "Tracker response too large" << endl;
            return;
        }
    }

    if (resp.empty())
    {
        cout << "Tracker read error - response is empty" << endl;
        return;
    }

    istringstream riss(resp);
    string line;

    if (!getline(riss, line))
    {
        cout << "Unexpected tracker response (empty)\n";
        return;
    }

    if (!line.empty() && line.back() == '\r')
        line.pop_back();
    istringstream h(line);
    string token;
    long filesize;
    string sha_full;
    int num_pieces;
    string filepath_on_peer;
    if (!(h >> token >> filesize >> sha_full >> num_pieces >> filepath_on_peer))
    {
        cout << "Malformed tracker header: " << line << "\n";
        return;
    }
    if (token != "FOUND")
    {
        cout << "Unexpected tracker token: " << token << "\n";
        return;
    }

    if (!getline(riss, line))
    {
        cout << "Malformed tracker response: missing piece hashes\n";
        return;
    }
    if (!line.empty() && line.back() == '\r')
        line.pop_back();
    vector<string> piece_hashes;
    {
        istringstream phs(line);
        string ph;
        while (phs >> ph)
            piece_hashes.push_back(ph);
    }

    if (!getline(riss, line))
    {
        cout << "Malformed tracker response: missing peers\n";
        return;
    }
    if (!line.empty() && line.back() == '\r')
        line.pop_back();
    vector<string> peers;
    {
        istringstream ps(line);
        string first;
        if (!(ps >> first))
        { /* no peers */
        }
        else
        {
            if (first == "PEERS")
            {
                string p;
                while (ps >> p)
                    peers.push_back(p);
            }
            else
            {
                peers.push_back(first);
                string p;
                while (ps >> p)
                    peers.push_back(p);
            }
        }
    }

    if (peers.empty())
    {
        cout << "No peers available\n";
        return;
    }

    auto task = make_shared<DownloadTask>();
    task->group_id = group;
    task->filename = filename;
    task->dest_path = dest_path;
    task->filesize = filesize;
    task->sha1_full = sha_full;
    task->piece_hashes = piece_hashes;
    task->num_pieces = (int)piece_hashes.size();
    task->piece_status.assign(task->num_pieces, 0);
    task->pieces_done = 0;

    {
        lock_guard<mutex> a(active_downloads_mutex);
        active_downloads[group + "|" + filename] = task;
    }

    string partname = dest_path + "/" + filename + ".part";
    int fd = open(partname.c_str(), O_CREAT | O_RDWR, 0666);
    if (fd < 0)
    {
        cout << "Failed to create part file at " << partname << "\n";
        return;
    }

    // Prepare piece queue
    queue<int> pending_pieces;
    for (int i = 0; i < task->num_pieces; i++)
        pending_pieces.push(i);
    mutex queue_mtx;

    auto worker = [&](int tid)
    {
        while (true)
        {

            if (task->cancel.load())
            {
                close(fd);
                remove(partname.c_str());
                return;
            }

            int piece = -1;
            {
                lock_guard<mutex> lg(queue_mtx);
                if (pending_pieces.empty())
                    break;
                piece = pending_pieces.front();
                pending_pieces.pop();
            }

            if (task->cancel.load())
            {
                close(fd);
                remove(partname.c_str());
                return;
            }

            int retries = 0;
            bool piece_done = false;

            while (!piece_done && retries < 5 && !task->cancel.load())
            {
                for (size_t pi = 0; pi < peers.size() && !piece_done; ++pi)
                {
                    int s = connect_to_peer(peers[(pi + tid) % peers.size()]);
                    if (s < 0)
                        continue;

                    string req = "get_piece " + filepath_on_peer + " " + to_string(piece) + "\n";
                    if (send_all(s, req.c_str(), req.size()) < 0)
                    {
                        close(s);
                        continue;
                    }

                    string hdr;
                    if (!recv_line(s, hdr))
                    {
                        close(s);
                        continue;
                    }
                    if (hdr.rfind("DATA", 0) != 0)
                    {
                        close(s);
                        continue;
                    }

                    size_t len = 0;
                    {
                        istringstream hh(hdr);
                        string tmp;
                        hh >> tmp >> len;
                    }

                    vector<char> pbuf(len);
                    if (recv_all(s, pbuf.data(), len) != (ssize_t)len)
                    {
                        close(s);
                        continue;
                    }
                    close(s);

                    // Compute SHA of received piece
                    string got = sha1_buf((unsigned char *)pbuf.data(), len);
                    if (got != task->piece_hashes[piece])
                    {
                        retries++;
                        continue;
                    }

                    // Determine correct offset
                    off_t offset = (off_t)piece * (512 * 1024);
                    ssize_t wr = pwrite(fd, pbuf.data(), len, offset);
                    if (wr != (ssize_t)len)
                    {
                        retries++;
                        continue;
                    }

                    // Mark piece done
                    task->piece_status[piece] = 1;
                    task->pieces_done++;
                    piece_done = true;

                    {
                        lock_guard<mutex> lg(queue_mtx);
                        double progress = (double)(piece + 1) / num_pieces * 100.0;
                        cout << "\rThread " << tid
                             << " downloading... " << fixed << setprecision(2)
                             << progress << "% completed" << flush;
                    }
                }

                if (!piece_done)
                {
                    retries++;
                    this_thread::sleep_for(chrono::milliseconds(500));
                }
            }

            if (!piece_done)
            {
                lock_guard<mutex> lg(queue_mtx);
                pending_pieces.push(piece); // retry later
            }
        }
    };

    int max_threads = min((int)peers.size() * 2, 8);
    vector<thread> pool;
    for (int i = 0; i < max_threads; i++)
        pool.emplace_back(worker, i);
    for (auto &th : pool)
        if (th.joinable())
            th.join();

    close(fd);

    if (task->cancel.load())
    {
        remove(partname.c_str());
        return;
    }
    // Compute final SHA
    int fd2 = open(partname.c_str(), O_RDONLY);
    if (fd2 < 0)
    {
        cout << "Open assembled file failed\n";
        return;
    }
    SHA_CTX ctx;
    SHA1_Init(&ctx);
    ssize_t rr;
    while ((rr = read(fd2, buf, sizeof(buf))) > 0)
        SHA1_Update(&ctx, buf, rr);
    close(fd2);

    unsigned char finalhash[SHA_DIGEST_LENGTH];
    SHA1_Final(finalhash, &ctx);
    char finalhex[SHA_DIGEST_LENGTH * 2 + 1];
    for (int i = 0; i < SHA_DIGEST_LENGTH; i++)
        sprintf(finalhex + i * 2, "%02x", finalhash[i]);
    finalhex[SHA_DIGEST_LENGTH * 2] = 0;

    if (string(finalhex) == task->sha1_full)
    {
        string finalname = dest_path + "/" + filename;
        rename(partname.c_str(), finalname.c_str());
        cout << "\nDownload complete: " << finalname << "\n";
        task->completed = true;
    }
    else
    {
        cout << "\nsFinal SHA mismatch. Download incomplete or corrupted.\n";
    }
}

void show_downloads()
{
    lock_guard<mutex> a(active_downloads_mutex);
    if (active_downloads.empty())
    {
        cout << "No downloads\n";
        return;
    }
    for (auto &kv : active_downloads)
    {
        auto &task = kv.second;
        int done = task->pieces_done.load();
        int total = task->num_pieces;
        int percent = (total > 0) ? (done * 100 / total) : 0;
        cout << task->filename << " (" << task->group_id << ") ";
        if (task->completed)
            cout << "Completed 100% [" << total << "/" << total << "]\n";
        else
            cout << percent << "% [" << done << "/" << total << "]\n";
    }
}

void list_files(int tracker_fd, const string &group_id)
{
    string msg = "list_files " + group_id + "\n";
    send(tracker_fd, msg.c_str(), msg.size(), 0);

    char buffer[4096] = {0};
    int bytes = read(tracker_fd, buffer, sizeof(buffer));
    if (bytes > 0)
        cout << "Tracker>>\n"
             << string(buffer, bytes) << endl;
    else
        cout << "RAW tracker response: " << string(buffer, bytes) << endl;
}

void stop_share(int tracker_fd, const string &group_id, const string &filename)
{
    string key = group_id + "|" + filename;
    shared_ptr<DownloadTask> task = nullptr;

    {
        lock_guard<mutex> lg(active_downloads_mutex);
        if (active_downloads.count(key))
        {
            task = active_downloads[key];
            task->cancel.store(true);
            cout << "Download cancelled: " << filename << endl;
        }
        else
        {
            cout << "No active download found for: " << filename << endl;
        }
    }

    // Wait a short time for threads to notice cancel flag
    if (task)
    {
        while (!task->completed && task->pieces_done.load() < task->num_pieces)
        {
            this_thread::sleep_for(chrono::milliseconds(100));
        }
        // Now safe to remove
        lock_guard<mutex> lg(active_downloads_mutex);
        active_downloads.erase(key);
    }

    // Notify tracker
    string msg = "stop_share " + group_id + " " + filename + "\n";
    send(tracker_fd, msg.c_str(), msg.size(), 0);

    char buffer[1024] = {0};
    int bytes = read(tracker_fd, buffer, sizeof(buffer));
    if (bytes > 0)
        cout << "Tracker>> " << string(buffer, bytes) << endl;
}

int main(int argc, char *argv[])
{
    if (argc != 3)
    {
        cout << "Usage: " << argv[0] << " <ip:port> tracker_info.txt" << endl;
        return 0;
    }
    string peer_info = argv[1];
    size_t pos = peer_info.find(':');
    if (pos == (size_t)-1)
    {
        cout << "Invalid peer info format. Use <ip:port>" << endl;
        return 0;
    }

    string peer_ip = peer_info.substr(0, pos);
    int peer_port;
    try
    {
        peer_port = stoi(peer_info.substr(pos + 1));
        if (peer_port < 1024 || peer_port > 65535)
        {
            cout << "Port number must be between 1024 and 65535" << endl;
            return 0;
        }
    }
    catch (exception &e)
    {
        cout << e.what() << endl;
    }

    char *filename = argv[2];

    int fd = open(filename, O_RDONLY);
    if (fd < 0)
    {
        perror("File open error");
        return 0;
    }

    char file_buffer[1024] = {0};
    int bytes = read(fd, file_buffer, 1024);
    close(fd);
    if (bytes <= 0)
    {
        perror("File read error");
        return 0;
    }

    file_buffer[bytes] = '\0';

    thread t(client_server, peer_port);
    t.detach();

    vector<pair<string, int>> trackers;
    istringstream iss(file_buffer);
    string ip;
    int port;
    while (iss >> ip >> port)
    {
        try
        {
            if (port < 1024 || port > 65535)
            {
                cout << "Port number must be between 1024 and 65535" << endl;
                continue;
            }
            trackers.push_back({ip, port});
        }
        catch (exception &e)
        {
            cout << e.what() << endl;
            return 0;
        }
    }

    if (trackers.size() == 0)
    {
        cout << "No valid tracker info found" << endl;
        return 0;
    }

    cout << "Available trackers:" << endl;
    for (auto &t : trackers)
    {
        cout << t.first << ":" << t.second << endl;
    }

    int tracker_fd = connect_to_tracker(trackers);

    while (1)
    {
        cout << ">> ";
        string command;
        getline(cin, command);
        if (command == "exit" || command == "quit")
        {
            string logout_msg = "logout\n";
            send(tracker_fd, logout_msg.c_str(), logout_msg.size(), 0);
            break;
        }
        if (command.rfind("login", 0) == 0)
        {
            command += " " + peer_ip + ":" + to_string(peer_port);
        }
        else if (command.rfind("upload_file", 0) == 0)
        {
            istringstream iss(command);
            string cmd, group_id, filepath;
            iss >> cmd >> group_id >> filepath;
            if (group_id.empty() || filepath.empty())
            {
                cout << "Usage: upload_file <group_id> <file_path>\n";
                continue;
            }
            upload_file(tracker_fd, group_id, filepath);
            continue;
        }
        else if (command.rfind("download_file", 0) == 0)
        {
            if (command.size() <= 14)
            {
                cout << "Usage: download_file <group_id> <file_name> <destination_path>" << endl;
                continue;
            }
            string filename = command.substr(14);
            download_file(tracker_fd, filename);
            continue;
        }
        else if (command.rfind("list_files", 0) == 0)
        {
            if (command.size() <= 11)
            {
                cout << "Usage: list_files <group_id>" << endl;
                continue;
            }
            string group_id = command.substr(11);
            list_files(tracker_fd, group_id);
            continue;
        }
        else if (command == "show_downloads")
        {
            show_downloads();
            continue;
        }
        else if (command.rfind("stop_share", 0) == 0)
        {
            istringstream iss(command);
            string cmd, group_id, filename;
            iss >> cmd >> group_id >> filename;
            if (group_id.empty() || filename.empty())
            {
                cout << "Usage: stop_share <group_id> <file_name>\n";
                continue;
            }
            stop_share(tracker_fd, group_id, filename);
            continue;
        }
        string message = command + "\n";
        send(tracker_fd, message.c_str(), message.size(), 0);
        char buffer[1024] = {0};
        int bytes = read(tracker_fd, buffer, 1024);
        if (bytes > 0)
        {
            cout << "Tracker>> " << buffer << endl;
        }
        else if (bytes == 0)
        {
            cout << "Connection to tracker lost" << endl;
            close(tracker_fd);
            tracker_fd = connect_to_tracker(trackers);
            cout << "Please re-login to continue." << endl;
            continue;
        }
        else
        {
            perror("Read error");
            close(tracker_fd);
            exit(0);
        }
    }
    close(tracker_fd);
    return 0;
}