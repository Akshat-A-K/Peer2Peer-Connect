# P2P File Sharing System with Redundant Trackers

Course: Advanced Operating System

Roll No: 2025201005


## Architecture overview

Components
- Tracker: accepts client connections, manages users/groups/files, and replicates state-changing events to a peer tracker using a persistent sync socket.
- Client: interactive CLI; registers with a tracker, issues commands (login, create_group, upload/download), and hosts a peer-server to serve file pieces.
- Tracker sync: a separate thread that connects to the peer tracker and applies incoming state-change messages.

Data model (in-memory)
- Users: `user_and_password` (username -> password)
- Active sessions / peer contact: `user_ports` (username -> ip:port or peer id)
- Client->username mapping: `client_user` (client socket fd -> username)
- Groups: `group_members`, `group_leader`, `group_requests`
- Files: `group_files` (group -> vector<FileInfo>), `all_files` (filepath -> FileInfo)

Concurrency
- Trackers use mutexes around user and file maps (`user_mutex`, `group_members_mutex`, `files_mutex`).
- Each incoming client connection is handled in its own thread (`thread-per-client`).
- Clients use worker threads to fetch file pieces in parallel; `DownloadTask` owns worker threads and an atomic cancellation flag.

---

## Protocols & formats (derived from code)

Tracker-client messages (examples)
- create_user <username> <password>
- login <username> <password> <peer-ip:peer-port>
- logout
- create_group <group_id>
- join_group <group_id>
- upload_file <group_id> <file_path>
- download_file <group_id> <file_name> <destination_path>
- list_files <group_id>
- stop_share <group_id> <file_name>

Tracker responses
- Most handlers return short plain-text responses. Download_file returns a multi-line response starting with `FOUND <filesize> <sha1_full> <num_pieces> <filepath>` followed by a line with space-separated piece SHA1 hashes and a `PEERS` line listing peers.

Client-peer interaction
- Clients open a peer-server that supports a minimal `get_piece <filepath> <piece_idx>` request. Responses are:
	- `DATA <len>\n` followed by raw bytes, or
	- `ERROR <reason>` for failures.

Chunking and hashing
- Piece size: 512 KiB (512*1024 bytes).
- Per-piece verification: each piece is SHA1 hashed using OpenSSL APIs; the full-file SHA1 is computed and checked after assembly.

Tracker-tracker sync
- Trackers maintain one persistent TCP connection to a peer. When a tracker handles a client command that mutates state it calls `send_sync_message(command)` (subject to small exclusions in the main loop). The peer's sync thread receives the command string and invokes the same handler locally while marking `is_sync_message` to avoid re-forwarding.

---

## How the client download works (key details)

- `download_file` asks the tracker for file metadata (size, full SHA1, piece hashes, peers).
- A `DownloadTask` is created with per-piece state and a queue of pending piece indices.
- Multiple worker threads are spawned (bounded by peers*2 and capped at 8). Each worker repeatedly picks a pending piece, connects to one of the peers, requests `get_piece`, receives `DATA`, verifies piece SHA1 and writes it to the correct offset in a `.part` file using `pwrite`.
- On completion the `.part` file is verified against the file-level SHA1 and, if correct, renamed to the final filename and the tracker is notified (`upload_file` to inform the tracker this peer now has the file).
- If the user issues `logout`, `stop_all_downloads()` sets a cancellation flag and joins worker threads; stopped filenames are printed to console.

---

## Build & run (concrete)

1. Build

```bash
cd Tracker
make
cd ../Client
make
```

2. Start two trackers (example)

Open two terminals and run, using the lines in `tracker_info.txt` and tracker numbers 1 and 2:

```bash
cd Tracker
./tracker ../tracker_info.txt 1
./tracker ../tracker_info.txt 2
```

3. Start a client

```bash
cd Client
./client 127.0.0.1:4001 ../tracker_info.txt
```

4. Common test flow (manual)

- On client A: `create_user u1 p1` -> `login u1 p1` (the client appends its peer ip:port automatically to the login token).
- On client A: `create_group g1` -> `upload_file g1 /path/to/file`
- On client B (connected to the other tracker): `download_file g1 filename /tmp` -> client B should obtain peers from tracker and fetch pieces.
- On client A: start a download then `logout` -> the client should cancel downloads and print `Stopped download: <filename>`.


---

## Where to look in the code

- `Client/client.cpp` — CLI, `DownloadTask` semantics, `client_server()` and peer `get_piece` handler.
- `Client/net_utils.h` — `send_all` / `recv_all` / `recv_line` helpers used throughout.
- `Tracker/tracker.cpp` — accept loop, `client_handle()` dispatch, and when `send_sync_message()` is invoked.
- `Tracker/tracker_sync.cpp` — persistent peer connection, receiving/dispatching sync messages.
- `Tracker/user.cpp` & `Tracker/files.cpp` — user/group/file handlers and shared in-memory maps.

---