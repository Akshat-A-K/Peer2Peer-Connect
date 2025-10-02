# Peer-to-Peer File Sharing System — Assignment 3

Roll No: 2025201005

This repository implements a P2P distributed file sharing system with two redundant trackers and clients that act as both downloaders and seeders. This README documents the full final submission: build/run steps, architecture, protocols, data structures, algorithms, testing instructions, assumptions, limitations, and the submission layout required by the assignment.

Implemented features
- Two redundant tracker servers that synchronize state updates.
- Client CLI supporting: create_user, login, create_group, join group, leave group, list groups, list requests, accept request, upload_file, list_files, download_file, show_downloads, stop_share, logout.
- Clients host a peer-server to serve pieces via `get_piece` requests.
- Parallel downloads using a lightweight thread-pool per download task. Multiple pieces are downloaded concurrently and workers prefer peers in a round-robin manner so different pieces favor different peers.
- Background downloads: `download_file` returns immediately — the actual download runs asynchronously in the background. `show_downloads` reports per-user download status.
- Piece-level and full-file SHA1 verification. Corrupted pieces are re-requested from other peers.
- Robust tracker-to-tracker synchronization using a persistent sync connection (state changes are forwarded and applied on the peer while avoiding loops).

Build & run
Prerequisites
- Linux environment with g++, make, and OpenSSL dev libs.

Build
```bash
cd Tracker
make
cd ../Client
make
```

Start trackers (example)
1) Open terminal A:
```bash
cd Tracker
./tracker ../tracker_info.txt 1
```
2) Open terminal B:
```bash
cd Tracker
./tracker ../tracker_info.txt 2
```

Start a client (example)
```bash
cd Client
./client 127.0.0.1:4001 ../tracker_info.txt
```

Notes
- The client CLI prompt is interactive. When issuing `login` the client auto-appends its peer IP:PORT.
- `download_file` returns immediately and starts the download in background.

CLI command reference (supported)
- create_user <user id> <password>
- login <user id> <password>
- create_group <group id>
- join_group <group id>
- leave_group <group id>
- list_groups
- list_requests <group id>
- accept_request <group id> <user id>
- upload_file <group id> <file_path>
- list_files <group id>
- download_file <group id> <file_name> <destination_path>
- show_downloads
- stop_share <group id> <file_name>
- logout

Quick example workflow
1) On client A:
	 create_user alice pass
	 login alice pass
	 create_group g1
	 upload_file g1 /path/to/large_file.bin
2) On client B (connected to other tracker):
	 create_user bob pass
	 login bob pass
	 download_file g1 large_file.bin /tmp
	 show_downloads   # prints one line per active download: <group_id> <file_name> <D|C>

Architecture & key data structures
- Trackers
	- In-memory maps (protected by mutexes): users, group_members, group_requests, group_files, all_files.
	- Each client connection handled by a dedicated thread.
	- Persistent tracker-to-tracker sync thread that applies state-change commands received from the peer.

- Client
	- Interactive CLI + peer server thread which serves `get_piece` requests.
	- DownloadTask struct (per download) contains: group_id, filename, dest_path, filesize, sha1_full, piece_hashes, piece_status vector, atomic pieces_done, vector<thread> workers, cancellation flag and a file_write_mutex.

Network protocols and message formats
- Tracker <-> Client (text-based):
	- Commands as plain text lines: `upload_file <group> <path>\n`, `download_file <group> <name> <dest>\n`, etc.
	- `download_file` reply format on success:
		- First line: `FOUND <filesize> <sha1_full> <num_pieces> <filepath_on_peer>`
		- Second line: space-separated per-piece SHA1 hashes (num_pieces tokens)
		- Third line: `PEERS <ip:port> ...` (or just a space-separated list)

- Client <-> Client (peer-server)
	- Request: `get_piece <filepath_on_peer> <piece_idx>\n`
	- Success response: `DATA <len>\n` followed by exactly <len> raw bytes
	- Error response: `ERROR <reason>\n`

Concurrency and thread-safety
- Each download runs a lightweight thread-pool (workers stored in the download task). Workers pull pending piece indices from a synchronized queue.
- To allow background downloads, `download_file` starts a detached manager thread which creates the worker threads and joins them when the download completes (workers themselves are joinable via stop_all_downloads()).
- File writes: pwrite was replaced with a safe lseek+write sequence protected by a per-download mutex (`file_write_mutex`) to avoid file-offset races and ensure portability.

Piece management, hashing and verification
- Piece size: 512 KB (512*1024 bytes). Final piece may be smaller.
- Each piece has a SHA1 hash. The tracker stores and distributes per-piece SHA1 strings and the full-file SHA1.
- Workers verify each received piece's SHA1. If verification fails, they retry requesting the piece from other peers (with a limited retry count). After all pieces are written, the assembled `.part` file is SHA1-checked against the full-file hash; if it matches the file is renamed and the tracker is informed.

Piece selection strategy
- Round-robin peer preference: for a given piece index p, the worker starts attempts from peer index (p % peers.size()) and cycles peers after that. This spreads load across peers and allows multiple peers to be used concurrently for different pieces.

Tracker synchronization design
- Each tracker maintains a persistent TCP connection to its peer and forwards state-changing commands (like `create_user`, `create_group`, `upload_file`) to the peer by sending the same command string over that socket. Incoming sync messages are applied locally while guarded by an `is_sync_message` flag to avoid re-forwarding loops.
- The sync channel is designed to survive temporary disconnections: on failure the tracker attempts reconnects and the local tracker continues operating; when the peer comes back online the implementer-provided sync can be extended to replay missed operations if required (current implementation forwards live updates and relies on both trackers being up for full redundancy).

Testing and validation
1) Build both trackers and two clients on the same machine (or separate machines).
2) Start both trackers and log their addresses in `tracker_info.txt`.
3) On client A: create_user, login, create_group, upload_file a reasonably large file (>= 2MB so multiple pieces exist).
4) On client B and C: login using different users, then start `download_file` for the same file (connect to different trackers if you want to exercise sync). Observe `show_downloads` and client logs. Workers will download different pieces concurrently and the console shows progress.


- Code compiles without errors; Makefiles provided.
- Piece size and hashing behavior: pieces are 512 KiB; per-piece and full-file SHA1 calculations present.
- Concurrent downloads: multiple worker threads fetch different pieces concurrently and use round-robin peer preference.
- Background downloads and `show_downloads` output: `download_file` returns immediately and `show_downloads` prints one-line-per-download with group, file and status C/D.



---