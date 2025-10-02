# Peer-to-Peer File Sharing System — Combined README & Technical Report

Roll No: 2025201005

This single document replaces separate README/technical-note files. It is concise, focused and contains all required information: build/run instructions, architecture, key data structures and algorithms, protocol formats, design rationale, assumptions, limitations, and testing steps.

## Quick start
Prerequisites: g++ (C++17), make, libssl-dev (OpenSSL headers), POSIX shell.

Build:
```bash
cd Tracker
make clean 
make
cd ../Client
make clean 
make
```

Prepare `tracker_info.txt` (root) with two lines `IP PORT` for trackers, e.g.:
```
127.0.0.1 4001
127.0.0.1 4002
```

Run:
```bash
# terminal A
cd Tracker
./tracker ../tracker_info.txt 1

# terminal B
cd Tracker
./tracker ../tracker_info.txt 2

# client 
cd Client
./client 127.0.0.1:4001 ../tracker_info.txt
```

## CLI commands
create_user <user_id> <password>
login <user_id> <password>
create_group <group_id>
join_group <group_id>
leave_group <group_id>
list_groups
list_requests <group_id>
accept_request <group_id> <user_id>
upload_file <group_id> <file_path>
list_files <group_id>
download_file <group_id> <file_name> <destination_path>
show_downloads
stop_share <group_id> <file_name>
logout

## Architecture
- Trackers: in-memory metadata (users, groups, files) with mutex protection; thread-per-connection model; persistent outbound sync socket + inbound accept for tracker-to-tracker replication.
- Clients: interactive CLI, background peer-server for `get_piece`, and per-download manager + worker threads for parallel piece fetching.

## Key data structures and rationale
- Tracker:
  - users: unordered_map<string,string> — O(1) auth lookups.
  - group_members: unordered_map<string, unordered_set<string>> — O(1) membership checks.
  - group_requests: unordered_map<string, vector<string>> — FIFO request listing.
  - group_files: unordered_map<string, vector<FileMeta>> — small-scale listing; FileMeta stores filename, size, full_sha1, piece_hashes, advertised path.
  - all_files: unordered_map<string, FileMetaFull> — global lookup for quick searches.

- Client:
  - DownloadTask contains metadata, vector<string> piece_hashes, piece_status vector, atomic<int> pieces_done, worker threads, cancel flag, and a file_write_mutex.

Rationale: unordered_map/unordered_set are fast and simple for assignment scale; vectors preserve display order.

## Algorithms & implementation highlights
- Piece size: 512 KB.
- Hashing: per-piece and full-file SHA-1 (OpenSSL) produced by uploader and stored at tracker.
- Download manager: `download_file` creates a manager thread that spawns N workers. Workers atomically claim piece indices and attempt peers in a round-robin starting index (p % peers.size()). Verified pieces are written under a file mutex; when all pieces complete, assembled file is SHA-1 checked and renamed.
- Retry: limited retries per piece; on verification failure the worker tries other peers.

## Synchronization algorithm 
- Each state-changing command is forwarded as a newline-terminated text line over the persistent sync socket.
- Incoming sync commands are applied locally with an `is_sync_message` guard to avoid re-forwarding; inbound accepted sync sockets are tracked to prevent loops.
- The approach is eventual replication for live updates; it does not provide durable replay or causal guarantees.

## Network protocol
- Tracker <-> Client (text lines): commands newline-terminated. Example: `upload_file g1 /abs/path\n`.
- `download_file` success response (three lines):
  1) `FOUND <filesize> <full_sha1> <num_pieces> <filepath_on_peer>\n`
  2) `<sha1_piece_0> <sha1_piece_1> ...\n`
  3) `PEERS <ip:port> ...\n`
- Peer <-> Peer:
  - Request: `get_piece <filepath_on_peer> <piece_idx>\n`
  - Success: `DATA <len>\n` followed by exactly <len> raw bytes.
  - Error: `ERROR <reason>\n`

Reliability notes: code uses an accumulator for socket streams and extracts newline-terminated messages to prevent partial-read parsing bugs.

## Implemented features
- User/group management (create/login/join/accept/list/leave).
- Upload/advertise files (compute piece hashes), stop_share.
- Background parallel downloads with piece verification and retries.
- Peer `get_piece` server supporting `DATA <len>` payloads.
- Tracker-to-tracker replication with loop-avoidance.

## Limitations & assumptions
- No authentication or signing for messages (trusted environment assumption).
- No durable operation log: if both trackers are offline, missed updates are not replayed.
- SHA-1 used due to assignment constraints; production code should use SHA-256.
- Designed for LAN/loopback testing; no NAT traversal.

## Testing 
1) Ensure `libssl-dev` installed and `tracker_info.txt` present in repo root with two tracker endpoints.
2) Build (see Quick start).
3) Start trackers in two terminals:
```bash
cd Tracker
./tracker ../tracker_info.txt 1
./tracker ../tracker_info.txt 2
```
4) Start seeder client and upload a file >= 2MB:
```bash
cd Client
./client 127.0.0.1:4001 ../tracker_info.txt
create_user seeder pass
login seeder pass
create_group g1
upload_file g1 /full/path/to/test.bin
```
5) Start downloader client and download:
```bash
cd Client
./client 127.0.0.1:4002 ../tracker_info.txt
create_user dl pass
login dl pass
join_group g1
download_file g1 test.bin /tmp/test.bin
show_downloads
```
6) Validate SHA-1 of `/tmp/test.bin` matches tracker metadata.

Edge-case tests: run multiple concurrent downloaders, simulate piece corruption (tamper bytes on seeder), and restart trackers to observe sync reconnect behavior.

## Troubleshooting
- If trackers log fragmented commands: ensure `tracker_info.txt` is correct and there is no network interference; the code accumulates socket bytes before parsing.
- For segmentation faults: run `gdb` on the tracker binary and inspect the last processed command; numeric parse exceptions and `DATA <len>` mismatches are common causes.

---
