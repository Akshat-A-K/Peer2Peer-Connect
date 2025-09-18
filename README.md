# Assignment 3 – Peer-to-Peer Distributed File Sharing System
Roll Number: 2025201005

## Overview
This project implements a **P2P Distributed File Sharing System** with redundant tracker servers.  
Clients can register, login, create/join groups, and manage group memberships. Trackers maintain consistent user and group metadata using a synchronization mechanism.

## Features Implemented
- **Client**
  - Connects to available trackers (fallback if one fails).
  - Runs a peer server for handling file requests (dummy file data for now).
  - Supports commands:
    - `create_user <username> <password>`
    - `login <username> <password>`
    - `logout`
    - `create_group <group_id>`
    - `join_group <group_id>`
    - `leave_group <group_id>`
    - `list_groups`
    - `list_requests <group_id>`
    - `accept_request <group_id> <username>`

- **Tracker**
  - Maintains user credentials, login sessions, group memberships, and requests.
  - Synchronizes updates with peer tracker:
    - User creation
    - Group creation
    - Join requests
    - Accepting/Leaving groups
  - Uses TCP sockets and threads for concurrency.

## Synchronization
- Two trackers run (`./tracker tracker_info.txt <tracker_no>`).  
- Each tracker starts a background sync thread that:
  - Connects to the other tracker.
  - Forwards commands that modify global state.
  - Prevents infinite loops using `is_sync_message` flag.
- This ensures **basic consistency** for user and group management.

**Note:** File-sharing operations (`upload_file`, `download_file`, etc.) are not yet synchronized.

## Execution
1. Start trackers:
   ```bash
   ./tracker tracker_info.txt 1
   ./tracker tracker_info.txt 2
   ```
2. Start a client:
   ```bash
   ./client <IP>:<PORT> tracker_info.txt
   ```
3. Use commands (see above) to interact with the system.
4. Type `exit` or `quit` in client to logout and stop.

## Limitations
- File upload/download and integrity checks are not yet implemented.
- Only group and user management commands are synchronized.
