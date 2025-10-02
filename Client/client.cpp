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

// Structure to hold download task information
struct DownloadTask
{
    string group_id;
    string filename;
    string dest_path;
    long filesize;
    string sha1_full;
    vector<string> piece_hashes;
    vector<char> piece_status; // 'N' = not started, 'D' = downloading, 'C' = completed
    atomic<int> pieces_done;
    int num_pieces;
    mutex m;
    bool completed = false;
    atomic<bool> cancelled{false};
    vector<thread> workers;
    atomic<bool> joined{false};
};

map<string, shared_ptr<DownloadTask>> active_downloads; // group id + filename -> task
mutex active_downloads_mutex;                           // protects active_downloads

// Track last successful login command so we can auto re-login after failover
string last_login_cmd = "";
bool logged_in = false;

// Get the size of a file
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

        // handle each accepted connection in a detached thread to allow concurrency
        thread([new_socket]()
               {
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
            close(new_socket); })
            .detach();
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
        cout << "Usage: download_file <group_id> <file_name> <destination_path>" << endl;
        return;
    }

    string msg = "download_file " + group + " " + filename + " " + dest_path + "\n";
    send(tracker_fd, msg.c_str(), msg.size(), 0);

    string resp;
    char buf[8192];
    while (1)
    {
        int n = read(tracker_fd, buf, sizeof(buf));
        if (n <= 0)
        {
            cout << "Tracker read error" << endl;
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
        cout << "Tracker response empty" << endl;
        return;
    }

    istringstream riss(resp);
    string line;

    if (!getline(riss, line))
    {
        cout << "Unexpected tracker response (empty)" << endl;
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
        cout << "Malformed tracker header: " << line << endl;
        return;
    }
    if (token != "FOUND")
    {
        cout << "File not found on tracker: " << token << endl;
        return;
    }

    if (!getline(riss, line))
    {
        cout << "Malformed tracker response: missing piece hashes" << endl;
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
        cout << "Malformed tracker response: missing peers" << endl;
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
        cout << "No peers available" << endl;
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
    task->piece_status.assign(task->num_pieces, 'N');
    task->pieces_done = 0;

    {
        lock_guard<mutex> a(active_downloads_mutex);
        active_downloads[group + "|" + filename] = task;
    }

    string partname = dest_path + "/" + filename + ".part";
    int fd = open(partname.c_str(), O_CREAT | O_RDWR, 0666);
    if (fd < 0)
    {
        cout << "Failed to create part file at " << partname << endl;
        return;
    }

    // Prepare piece queue and a lightweight thread-pool
    queue<int> pending_pieces;
    for (int i = 0; i < task->num_pieces; i++)
        pending_pieces.push(i);

    mutex queue_mtx;
    condition_variable queue_cv;
    atomic<bool> stop_pool{false};

    int num_threads = 6; // base on CPU cores
    // scale with available peers but cap to a reasonable number
    num_threads = min(num_threads * 2, max(1, (int)peers.size() * 2));
    num_threads = min(num_threads, 32);

    // create worker threads and store them in task->workers so stop_all_downloads can join
    for (int i = 0; i < num_threads; ++i)
    {
        task->workers.emplace_back([&, i]()
                                   {
            const size_t CHUNK = 512 * 1024;
            while (true)
            {
                int piece = -1;
                {
                    unique_lock<mutex> lk(queue_mtx);
                    queue_cv.wait(lk, [&]() { return stop_pool.load() || !pending_pieces.empty(); });
                    if (stop_pool.load() && pending_pieces.empty())
                        break;
                    if (pending_pieces.empty())
                        continue;
                    piece = pending_pieces.front();
                    pending_pieces.pop();
                }

                if (task->cancelled)
                    break;

                // skip if another thread already completed this piece
                {
                    lock_guard<mutex> lg(task->m);
                    if (task->piece_status[piece] == 'C')
                        continue;
                    // mark as downloading
                    task->piece_status[piece] = 'D';
                }

                int retries = 0;
                bool piece_done = false;
                while (!piece_done && retries < 5 && !task->cancelled)
                {
                    // start from a peer chosen by piece index so different pieces prefer different peers
                    size_t start_peer = 0;
                    if (!peers.empty())
                        start_peer = (size_t)piece % peers.size();
                    for (size_t pi = 0; pi < peers.size() && !piece_done; ++pi)
                    {
                        size_t peer_idx = (start_peer + pi) % peers.size();
                        int s = connect_to_peer(peers[peer_idx]);
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

                        // Determine correct offset and write
                        off_t offset = (off_t)piece * (off_t)CHUNK;
                        ssize_t wr = pwrite(fd, pbuf.data(), len, offset);
                        if (wr != (ssize_t)len)
                        {
                            retries++;
                            continue;
                        }

                        // Mark piece done
                        {
                            lock_guard<mutex> lg(task->m);
                            if (task->piece_status[piece] != 'C')
                            {
                                task->piece_status[piece] = 'C';
                                task->pieces_done++;
                            }
                        }
                        piece_done = true;

                        // progress print (outside critical section)
                        double progress = (double)task->pieces_done.load() / task->num_pieces * 100.0;
                        cout << "\rThread " << i << " downloading... " << fixed << setprecision(2) << progress << "%" << flush;
                    }

                    if (!piece_done)
                    {
                        retries++;
                        this_thread::sleep_for(chrono::milliseconds(500));
                    }
                }

                if (!piece_done && !task->cancelled)
                {
                    // give up after retries: re-enqueue once for another chance
                    {
                        lock_guard<mutex> lg(queue_mtx);
                        // mark as not started again so another thread may pick it
                        {
                            lock_guard<mutex> lg2(task->m);
                            if (task->piece_status[piece] != 'C')
                                task->piece_status[piece] = 'N';
                        }
                        pending_pieces.push(piece);
                        queue_cv.notify_one();
                    }
                }
            } });
    }

    // notify workers that tasks are available
    queue_cv.notify_all();

    // Wait for completion or cancellation
    while (!task->cancelled && (int)task->pieces_done.load() < task->num_pieces)
    {
        this_thread::sleep_for(chrono::milliseconds(200));
    }

    // stop pool and join
    {
        stop_pool = true;
        queue_cv.notify_all();
    }
    for (auto &th : task->workers)
        if (th.joinable())
            th.join();
    task->joined = true;

    close(fd);

    int fd2 = open(partname.c_str(), O_RDONLY);
    if (fd2 < 0)
    {
        cout << "Open assembled file failed" << endl;
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
        cout << "Download complete: " << finalname << endl;
        task->completed = true;
        {
            lock_guard<mutex> lg(active_downloads_mutex);
            active_downloads.erase(task->group_id + "|" + task->filename);
        }
        upload_file(tracker_fd, task->group_id, dest_path + "/" + task->filename);
    }
    else
    {
        cout << "Final SHA mismatch. Download incomplete or corrupted." << endl;
    }
}

void show_downloads()
{
    lock_guard<mutex> a(active_downloads_mutex);
    if (active_downloads.empty())
    {
        cout << "No downloads" << endl;
        return;
    }
        for (auto &kv : active_downloads)
        {
            auto &task = kv.second;
            int total = task->num_pieces;
            int downloading = 0, completed = 0;
            {
                lock_guard<mutex> lg(task->m);
                for (char s : task->piece_status)
                {
                    if (s == 'D')
                        downloading++;
                    else if (s == 'C')
                        completed++;
                }
            }
            cout << task->filename << " (" << task->group_id << ") ";
            if (task->completed)
            {
                cout << "C 100% [" << completed << "/" << total << "]" << endl;
            }
            else
            {
                // Show 'D' if actively downloading, else show progress percent
                if (downloading > 0)
                    cout << "D " << fixed << setprecision(2) << ((double)completed / total * 100.0) << "% [C=" << completed << " D=" << downloading << "/" << total << "]" << endl;
                else
                    cout << ((total > 0) ? (completed * 100 / total) : 0) << "% [C=" << completed << " D=" << downloading << "/" << total << "]" << endl;
            }
        }
}

void stop_all_downloads()
{
    // signal cancellation first
    {
        lock_guard<mutex> a(active_downloads_mutex);
        for (auto &kv : active_downloads)
        {
            kv.second->cancelled = true;
        }
    }

    // join worker threads and report stopped downloads
    vector<string> stopped_files;
    {
        lock_guard<mutex> a(active_downloads_mutex);
        for (auto &kv : active_downloads)
        {
            auto &task = kv.second;
            if (task->completed)
                continue;

            // join workers if not already joined
            if (!task->joined)
            {
                for (auto &th : task->workers)
                {
                    if (th.joinable())
                        th.join();
                }
                task->joined = true;
            }

            // if still not completed, consider it stopped
            if (!task->completed)
                stopped_files.push_back(task->filename);
        }
    }

    for (auto &f : stopped_files)
        cout << "Stopped download: " << f << endl;
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
    bool stay = true;

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
        if (!getline(cin, command))
        {
            if (stay)
            {
                // keep the client alive to serve peers; check every second
                this_thread::sleep_for(chrono::seconds(1));
                continue;
            }
            else
            {
                break; // EOF and not staying
            }
        }
        if (command == "exit" || command == "quit")
        {
            string logout_msg = "logout\n";
            send(tracker_fd, logout_msg.c_str(), logout_msg.size(), 0);
            break;
        }
        if (command == "logout")
        {
            // stop active downloads before logging out
            stop_all_downloads();
            string logout_msg = "logout\n";
            send(tracker_fd, logout_msg.c_str(), logout_msg.size(), 0);
            cout << "Logged out and stopped active downloads" << endl;
            continue;
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
            string resp(buffer, bytes);
            cout << "Tracker>> " << resp << endl;
            // if this was a login and succeeded, remember it for auto re-login
            if (command.rfind("login", 0) == 0)
            {
                if (resp.find("Login successful") != string::npos)
                {
                    // command may have had peer appended; store only first three tokens
                    istringstream lis(command);
                    string t1, t2, t3;
                    lis >> t1 >> t2 >> t3;
                    last_login_cmd = t1 + " " + t2 + " " + t3;
                    logged_in = true;
                }
            }
        }
        else if (bytes == 0)
        {
            cout << "Connection to tracker lost" << endl;
            close(tracker_fd);
            tracker_fd = connect_to_tracker(trackers);
            cout << "Connected to new tracker. ";
            // attempt auto re-login if we had a previous successful login
            if (!last_login_cmd.empty())
            {
                cout << "Attempting auto re-login..." << endl;
                string login_cmd = last_login_cmd + " " + peer_ip + ":" + to_string(peer_port) + "\n";
                send(tracker_fd, login_cmd.c_str(), login_cmd.size(), 0);
                char lb[1024] = {0};
                int lb_bytes = read(tracker_fd, lb, sizeof(lb));
                if (lb_bytes > 0)
                {
                    string lresp(lb, lb_bytes);
                    cout << "Tracker>> " << lresp << endl;
                    if (lresp.find("Login successful") != string::npos)
                    {
                        cout << "Re-login succeeded." << endl;
                        logged_in = true;
                    }
                    else
                    {
                        cout << "Re-login failed: please login manually." << endl;
                        logged_in = false;
                    }
                }
                else
                {
                    cout << "No response after re-login attempt." << endl;
                    logged_in = false;
                }
            }
            else
            {
                cout << "Please login to continue." << endl;
            }
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