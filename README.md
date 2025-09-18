# P2P File Sharing System with Redundant Trackers

## Overview
This project implements a **peer-to-peer (P2P) file sharing system** with **redundant trackers**.  
Clients connect to a tracker for coordination (user login, group management, metadata exchange) but also act as mini-servers for other peers.  
Multiple trackers run in parallel and synchronize their states to ensure fault tolerance.

---

##  Client Side (client.cpp)

### `connect_to_tracker()`
- Reads a list of tracker IPs/ports (from `tracker_info.txt`).
- Iterates through them until one connection succeeds.
- Returns the connected socket descriptor.
- If none are available, exits.

 **Why this approach?**  
This ensures **failover capability**. Even if one tracker is down, clients can still join the network through another tracker.

---

### `client_server(peer_port)`
- Creates a local listening socket bound to the given `peer_port`.
- Accepts connections from other peers (simulating file sharing).
- Responds with dummy file data (in a real system, this would be file chunks).

 **Why this approach?**  
This turns every client into both a **server** and a **client**, enabling true **peer-to-peer** interaction.

---

### `main()`
- Takes input arguments:
  - `<ip:port>` → Peer info (for client server).
  - `tracker_info.txt` → List of trackers.
- Starts a background **peer server thread**.
- Reads available trackers and connects using `connect_to_tracker()`.
- Runs a **command loop** where users can:
  - `login <user> <pass>`
  - `create_user`, `create_group`, `logout`, etc.
- If the tracker disconnects, it **attempts reconnection** to another tracker automatically.

 **Why this approach?**  
Clients remain **self-sufficient**, capable of recovery without manual restart.

---

##  Tracker Side (tracker.cpp)

### Client Handling
- Listens for client connections.
- Spawns a dedicated thread (`client_handle`) for each client.
- Parses incoming commands:
  - `create_user`, `login`, `logout`
  - `create_group`, `join_group`, `leave_group`
  - `upload_file`, `download_file`, etc.
- Uses shared maps (from `user.cpp`) to manage:
  - Users & passwords
  - User login states
  - Groups & members
  - File metadata

 **Why this approach?**  
Thread-per-client allows **parallel handling**. Shared maps ensure a **single source of truth**.

---

##  Tracker Synchronization (tracker_sync.cpp / tracker_sync.h)

### `start_sync_thread(peer_ip, peer_port)`
- Connects to another tracker.
- Listens for **synchronization messages**.
- On receiving a message, applies the same operation locally (unless it was already processed).

### `send_sync_message(msg)`
- When a tracker processes a **new client command**, it forwards it to all peer trackers.
- Sync messages are tagged to prevent loops.

 **Why this approach?**  
This achieves **event-driven replication**.  
Instead of full snapshot copying (which is costly), only **state-changing events** are shared, keeping trackers consistent with minimal overhead.

---

##  User Management (user.cpp)

- Functions:
  - `create_user`
  - `login`
  - `logout`
  - `create_group`
  - `join_group`
  - etc.
- Metadata stored in:
  - `unordered_map<string, string>` → user → password
  - `unordered_map<string, int>` → user → port
  - `unordered_map<string, vector<string>>` → group → members
- Mutexes ensure **thread safety**.

 **Why this approach?**  
Maps provide **fast O(1) lookups**, suitable for frequent queries.  
Mutexes prevent race conditions when multiple clients modify shared state.

---

##  Metadata Organization

- **Users** → `user_and_password`
- **Active sessions** → `user_ports`
- **Groups** → `group_members`
- **Pending requests** → `join_requests`
- **Files** → (planned extension) mapping of group → file → list of peers.

 **Why this approach?**  
Organizing data into **logical maps** makes it easy to:
- Look up users quickly.
- Add/remove group members efficiently.
- Extend to file-sharing without major redesign.

---

##  Build Setup

### Client
```bash
cd Client
make
./client <ip:port> ../tracker_info.txt
```

### Tracker
```bash
cd Tracker
make
./tracker ../tracker_info.txt <tracker-number>
```

---

##  Design & Justification

1. **Synchronization**  
   - **Event-driven updates** (send only state-changing commands).

2. **Handling Connections**  
   - Thread-per-client model for simplicity.
   - Ensures each client gets responsive handling.
   - Future improvement: Thread pool to scale better.

3. **Metadata Storage**  
   - Used **unordered_maps** for O(1) lookups.
   - Mutexes ensure thread-safe concurrent access.
   - Easy to extend with new features (file metadata, permissions, etc.).

4. **Fault Tolerance**  
   - Clients cycle through available trackers.
   - Trackers replicate state to each other.
   - Ensures no single point of failure.

---

## Conclusion
This system demonstrates a **fault-tolerant P2P file sharing architecture** with:
- **Clients** that can act as peers and servers.
- **Trackers** that synchronize via event-driven updates.
- **Thread-safe metadata management**.
- **Resilience** against tracker failures.

The chosen design strikes a balance between **simplicity, performance, and reliability**, making it a strong foundation for a distributed file-sharing platform.
