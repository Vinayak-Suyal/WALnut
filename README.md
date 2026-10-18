# 🌰 WALnut: A Concurrent Auction Engine with Deadlock Detection & Recovery

**Course Project**: `OSDBMS-V-2026-T038`  
**Team**: **The Acid Mechanics** (`T038`)  
**Institution**: Graphic Era (Deemed to be University)

---

## Student & Team Information

| Team Member | Role | Student ID | Email | Primary Responsibilities |
|---|---|---|---|---|
| **Suyal, Vinayak** | **Team Lead** | `240211122` | `suyalvinayak22@gmail.com` | Storage manager, 4KB slotted-page binary layout, file manager, buffer pool manager (Clock replacement) |
| **Singh Rawat, Sushmit** | **Member** | `24021051` | `7838sushmit@gmail.com` | Transaction manager, Strict 2PL lock manager, dynamic wait-for graph (WFG) deadlock detection & victim rollback |
| **Rawat, Kanak** | **Member** | `240211907` | `kavyarawatkavya831@gmail.com` | Write-Ahead Logging (WAL), ARIES 3-phase recovery manager, fuzzy checkpointer, concurrent hash index, test suites |
| **Jugran, Suhavi** | **Member** | `240211640` | `suhavijugran@gmail.com` | Fixed operation API, embedded C++ REST & SSE server (`cpp-httplib`), React 19 dashboard, Engine Explorer visualizers, offline hotspot demo |

---

## Project Motivation & Philosophy

Modern applications rely on database management systems to store and process large volumes of data safely, even when many concurrent users access and mutate that data simultaneously. 

WALnut is motivated by a **live auction house**:
- Multiple bidders compete concurrently for the same auction items.
- The system must guarantee that every accepted bid is serialized correctly.
- No two simultaneous bids can both be recorded as the winning bid.
- An abrupt crash or power loss must never corrupt or lose a committed bid, nor leave an in-flight bid partially applied.

Rather than building a complex, general-purpose SQL relational engine, **WALnut deliberately unites core Operating Systems principles with fundamental Database Management Systems (DBMS) internals** around a fixed, highly observable workload:

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                       WALnut Core Engineering Fusion                        │
├──────────────────────────────────────┬──────────────────────────────────────┤
│         OPERATING SYSTEMS            │      DATABASE MANAGEMENT SYSTEMS     │
├──────────────────────────────────────┼──────────────────────────────────────┤
│ • POSIX / C++17 Threading            │ • Slotted-Page Storage Format        │
│ • Mutexes & Condition Variables      │ • Concurrent Extendible Hash Index   │
│ • Frame-Based Memory Caching         │ • Strict Two-Phase Locking (2PL)     │
│ • Clock (Second-Chance) Replacement  │ • ACID Transaction State Machine     │
│ • Wait-For Graph Deadlock Detection  │ • Write-Ahead Logging (WAL)          │
│ • Cycle Detection (Tarjan's / DFS)   │ • ARIES Crash Recovery (Redo / Undo) │
└──────────────────────────────────────┴──────────────────────────────────────┘
```

By constraining the workload to a parameterized fixed-operation API (`createUser`, `createItem`, `placeBid`, `getHighestBid`, `listBids`, `closeAuction`, and the multi-lock `reserveBundle`), every lock, frame eviction, wait-edge, and log record is 100% deterministic, measurable, and inspectable through a live visual dashboard.

---

## High-Level System Architecture

*(Derived from Project Proposal Page 5 and implemented across the C++ engine and React client)*

```
                       ┌─────────────────────────────────────────────────────────┐
                       │               React 19 + TypeScript Client              │
                       │   • Live Auction Floor         • Host Management Console│
                       │   • Engine Explorer Visualizer • Database Admin Portal  │
                       └────────────────────────────┬────────────────────────────┘
                                                    │ HTTP REST / Server-Sent Events (SSE)
                                                    ▼
                       ┌─────────────────────────────────────────────────────────┐
                       │               Embedded C++ API Layer                    │
                       │           cpp-httplib Server on 0.0.0.0:8080            │
                       │   • JSON REST Endpoints   • Real-Time SSE Event Stream  │
                       └────────────────────────────┬────────────────────────────┘
                                                    │
                                                    ▼
                       ┌─────────────────────────────────────────────────────────┐
                       │              Auction Operations (Fixed API)             │
                       │  createUser  createItem  placeBid  getHighestBid        │
                       │  listBids    closeAuction          reserveBundle        │
                       └────────────────────────────┬────────────────────────────┘
                                                    │
             ┌──────────────────────────────────────┴──────────────────────────────────────┐
             │                          Database Engine Core                               │
             ├──────────────────────────────────────┬──────────────────────────────────────┤
             │      Concurrency & Transactions      │         Durability & Recovery        │
             │  • TransactionManager (ACID states)  │  • LogManager (Append-only WAL)      │
             │  • LockManager (Strict 2PL, S / X)   │  • RecoveryManager (ARIES 3-Phase)   │
             │  • DeadlockDetector (Wait-For Graph) │  • Checkpointer (Fuzzy Checkpoints)  │
             ├──────────────────────────────────────┴──────────────────────────────────────┤
             │                             Indexing & Caching                              │
             │  • HashIndex (Concurrent Extendible Hashing, per-bucket read/write mutexes) │
             │  • BufferPool (Clock Second-Chance replacement, pin counting, dirty flags)  │
             ├─────────────────────────────────────────────────────────────────────────────┤
             │                          Physical Storage Manager                           │
             │  • 4096-Byte Slotted Page Format (64-byte Header, Slot Directory, CRC32)    │
             │  • FileManager (Direct OS File I/O, page allocation, sync)                  │
             └──────────────────────────────────────┬──────────────────────────────────────┘
                                                    │ Direct File I/O
                                                    ▼
                       ┌─────────────────────────────────────────────────────────┐
                       │               Physical Persistent Storage               │
                       │   • data/walnut.db   (Slotted Database Pages)           │
                       │   • data/walnut.wal  (Write-Ahead Log Stream)           │
                       │   • data/walnut.meta (Database Master Catalog)          │
                       │   • data/uploads/    (Product Photos)                   │
                       └─────────────────────────────────────────────────────────┘
```

---

## Database Internals: Subsystem Specifications

### 1. Slotted-Page Physical Storage (`src/storage/`)

All disk input/output occurs strictly in units of fixed **4,096-byte pages** (`PAGE_SIZE = 4096`). Each page contains a fixed **64-byte Page Header** followed by a 4,032-byte data region:

```
  0               16              32              48              64 Bytes
 ┌───────────────┬───────────────┬───────────────┬───────────────┐
 │  page_id (4B) │ page_type(4B) │rec_count (4B) │free_offset(4B)│
 ├───────────────┼───────────────┴───────────────┴───────────────┤
 │ checksum (4B) │ reserved[0..2] (12B)                          │
 ├───────────────┴───────────────────────────────────────────────┤
 │ padding (32B zeroed to fill PAGE_HEADER_SIZE = 64)            │
 ├───────────────────────────────────────────────────────────────┤ <-- Offset 64
 │ Slot 0 / Record 0                                             │
 │ Slot 1 / Record 1                                             │
 │ ...                                                           │
 │                        FREE SPACE REGION                      │
 │                                                               │
 │ ...                                                           │
 │ Slot N / Record N                                             │
 └───────────────────────────────────────────────────────────────┘ <-- Offset 4096
```

- **Compile-Time Alignment Guarantees** (`include/walnut/common/constants.hpp`):
  - `PAGE_SIZE = 4096`, `PAGE_HEADER_SIZE = 64`, `PAGE_DATA_SIZE = 4032`.
  - `USER_RECORD_SIZE = 280` bytes $\to$ Exactly 14 users per page (`4032 / 280 = 14`).
  - `ITEM_RECORD_SIZE = 800` bytes $\to$ Exactly 5 items per page (`4032 / 800 = 5`).
  - `TOP_BID_SLOT_SIZE = 112` bytes $\to$ 3 top bids are embedded directly inside each item record for instantaneous $O(1)$ auction state lookups without secondary table scans.
  - `BID_RECORD_SIZE = 176` bytes $\to$ Full audit-trail bid history.
  - Page Types: `PAGE_TYPE_USERS = 1`, `PAGE_TYPE_ITEMS = 2`, `PAGE_TYPE_BIDS = 3`, `PAGE_TYPE_METADATA = 4`, `PAGE_TYPE_FREE = 5`.
- **Integrity**: Every page contains an integrity checksum computed via seed `0xDEADBEEF`. Corrupted or partial writes are caught immediately upon `FileManager::readPage()`.

### 2. Buffer Pool Manager & Clock Replacement (`src/buffer/`)

The buffer pool retains active pages in a fixed collection of memory frames (default 64 frames):
- **Clock (Second-Chance) Replacement**:
  - Each frame tracks `page_id`, `pin_count`, `dirty` flag, and a `ref_bit`.
  - When an eviction is needed, the clock hand iterates through the frame ring:
    - If `pin_count > 0` (page currently held by an active transaction), the frame is skipped.
    - If `ref_bit == 1`, the reference bit is cleared to `0` ("second chance") and the hand advances.
    - If `ref_bit == 0`, the frame is selected as the eviction victim.
- **STEAL / NO-FORCE Buffer Policy**:
  - **STEAL**: Active dirty pages can be written to disk to free buffer frames.
  - **WAL Rule**: If an evicted victim frame is dirty (`dirty == true`), WALnut enforces the Write-Ahead Logging invariant: log records up to the page's LSN must be flushed to disk before the dirty page is flushed to `walnut.db`.

### 3. Concurrent Extendible Hash Index (`src/index/`)

- Provides $O(1)$ average-case indexing for `user_id` and `item_id` lookups to their physical `IndexRecordLocation` (`page_id`, `slot_id`).
- Implements dynamic hashing with global directory depth and bucket local depths.
- Supports fine-grained read/write locking via `std::shared_mutex` on buckets, allowing concurrent non-conflicting reads and updates.
- Supports rebuilding directly from raw disk (`BuildFromDisk`) during recovery or system bootstrap.

### 4. Transactions & Strict Two-Phase Locking (`src/txn/`)

WALnut transactions adhere strictly to ACID properties:
- **Lock Modes**: Shared (`LockMode::SHARED`, S) for reads, Exclusive (`LockMode::EXCLUSIVE`, X) for writes and bids.
- **Strict Two-Phase Locking (Strict 2PL)**:
  1. **Growing Phase**: Transactions acquire locks as needed. Lock upgrades from $S \to X$ are queued.
  2. **Lock Point**: The instant when all required locks have been acquired.
  3. **Shrinking Phase**: **All locks are held until transaction completion** (`COMMIT` or `ABORT`).
- **Isolation Guarantee**: Prevents dirty reads, non-repeatable reads, lost updates, and eliminates cascading rollbacks.

### 5. Deadlock Detection & Resolution (`src/txn/`)

- **Wait-For-Graph (WFG)**: `LockManager::BuildWaitForGraph()` constructs a directed graph $G = (V, E)$ where nodes represent active transactions and edges $(T_A \to T_B)$ represent $T_A$ blocked waiting for a lock held by $T_B$.
- **Background Detector**: A dedicated background thread runs every 500ms (`DeadlockDetector::WorkerLoop()`).
- **Cycle Detection**: Executes Depth-First Search (DFS) / Tarjan's cycle detection.
- **Victim Selection**: When a cycle is detected, WALnut selects the youngest transaction with the lowest invested cost as the victim, triggers `TransactionAbortedException`, rolls back its changes, and releases all its locks to unblock competing transactions.
- **Exercised via `reserveBundle`**: A specialized multi-item operation allowing users to bid on groups of items on an all-or-nothing basis, intentionally acquiring locks in conflicting orders to test deadlock detection and victim rollback under high load.

### 6. Write-Ahead Logging & ARIES Crash Recovery (`src/wal/`)

WALnut implements the classic **ARIES** recovery paradigm:
- **Append-Only Log** (`walnut.wal`): Log records contain:
  - `lsn`: Monotonic 64-bit Log Sequence Number.
  - `prev_lsn`: Previous LSN for the same transaction (thread backward).
  - `txn_id`: Transaction identifier.
  - `type`: `BEGIN`, `UPDATE`, `COMMIT`, `ABORT`, `CLR`, `CHECKPOINT`.
  - `page_id`, `offset`, `length`: Physical delta target.
  - `before_image`: Original bytes (for Undo).
  - `after_image`: New bytes (for Redo).
  - `undo_next_lsn`: Pointer for Compensation Log Records (CLRs).
- **ARIES 3-Phase Recovery Protocol**:
  1. **Analysis Phase**: Scans the WAL forward from the latest checkpoint to reconstruct the Dirty Page Table (DPT) and Transaction Table (TT). Identifies active loser transactions at crash time.
  2. **Redo Phase (Repeating History)**: Scans forward from the smallest `recLSN` in the DPT and reapplies all logged updates to restore the exact state of disk pages at the time of the crash.
  3. **Undo Phase**: Processes loser transactions backwards from highest LSN down to lowest, undoing modifications by copying `before_image` back to disk pages and logging `CLR` records.

### 7. Embedded REST API & Server-Sent Events (`src/server/`)

- High-throughput embedded C++ HTTP server powered by `cpp-httplib`.
- Binds to `0.0.0.0:8080`, allowing simultaneous connections from `localhost` and local Wi-Fi / LAN clients.
- Thread-safe Server-Sent Events (SSE) dispatcher (`/api/stream`) broadcasting live real-time bid updates, lot cancellations, lot closures, and database resets.
- Multipart form handler for lot image uploads with file type whitelist (JPEG, PNG, WebP, GIF) and 5 MB cap.
- Direct static file server hosting the React single-page application from `./dashboard/dist/`.

---

## Interactive Dashboard & Engine Explorer

The React 19 + TypeScript dashboard provides both a bidding client and an interactive educational visualizer of database internals:

| View | Route | Key Features |
|---|---|---|
| **Auction Floor** | `/` | Real-time live bidding, card layout, synchronized auction countdown timers, live SSE bid notifications, high-bidder badge, image lightboxes. |
| **Engine Explorer** | `/explorer` | Deep-dive database internals console with 6 interactive live visualizers: |
| ↳ *Clock Buffer Pool* | `/explorer` | 8-frame circular ring visualizer with clock hand pointer, pin count, dirty bits, reference bits, and step-by-step eviction simulator. |
| ↳ *Live Ledger* | `/explorer` | Live high-contrast log of all database transactions, SQL/binary equivalents, LSN tracking, and SSE push events. |
| ↳ *Strict 2PL Journey* | `/explorer` | Interactive timeline stepping through Growing Phase, Lock Point, WAL Flush, Shrinking Phase, and Commit. |
| ↳ *Live ARIES Simulator* | `/explorer` | Triggers a live C++ ARIES recovery sandbox (`/api/recovery/simulate-live`), executing real Analysis, Redo, and Undo with CLRs on temporary disk files. |
| ↳ *Hash Index Visualizer*| `/explorer` | Visual inspection of extendible hashing directory, bucket local/global depths, and slot allocation. |
| ↳ *Deadlock WFG* | `/explorer` | Live Wait-For-Graph cycle visualizer demonstrating conflict cycles and automatic victim abortion. |
| **Host Console** | `/host` | Publish auction lots with up to 3 photos, manage active lots, end/cancel lots, and display the **Audience Pairing QR Code**. |
| **Database Admin** | `/admin` | Diagnostic metrics (buffer pool hits/misses, storage pages), user management, item purging, and factory reset. |
| **Won Items** | `/won` | Overview of all auction lots successfully won by the logged-in user. |
| **User Profile** | `/profile` | Manage avatar URL, display name, and password changes. |

---

## Prerequisites & Dependencies

### Hardware
- Any standard x86_64 or ARM64 computer (Linux, macOS, or Windows).
- Minimum 2 GB RAM (4 GB recommended).
- ~500 MB free disk space for build outputs and database files.

### Software Requirements
| Tool | Minimum Version | Installation |
|---|---|---|
| **CMake** | $\ge 3.16$ | `sudo apt install cmake` / `brew install cmake` / `winget install Kitware.CMake` |
| **C++ Compiler** | C++17 compliant | `g++ >= 9`, `clang++ >= 10`, or `MSVC >= 2019` |
| **Node.js & npm** | Node $\ge 18$, npm $\ge 9$ | `sudo apt install nodejs npm` / `brew install node` / `winget install OpenJS.NodeJS` |
| **Git** | Any modern version | `sudo apt install git` / `brew install git` |

---

## Step-by-Step Setup & Build Instructions

### Step 1: Clone the Repository
```bash
git clone https://github.com/Vinayak-Suyal/WALnut.git
cd WALnut
```

---

### Step 2: Build the Frontend Dashboard
```bash
cd dashboard
npm install
npm run build
cd ..
```
*Note: This generates the production single-page application bundle inside `dashboard/dist/`, which `walnut_server` serves automatically.*

---

### Step 3: Build the C++ Database Engine

#### On Linux / macOS (GCC / Clang + Make or Ninja):
```bash
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . -j$(nproc 2>/dev/null || sysctl -n hw.ncpu)
cd ..
```

#### On Windows (PowerShell + Visual Studio):
```powershell
cmake -B build
cmake --build build --config Release
```

The build produces three executables:
- `walnut_server`: Embedded REST API + SSE server and dashboard web host
- `walnut_demo`: Standalone storage and buffer pool demo CLI
- `walnut_tests`: GoogleTest unit and integration test runner

---

## Running WALnut

### 1. Production Mode (Single Process — Recommended)
Run the server executable directly from the repository root:

```bash
# On Linux / macOS
./build/walnut_server ./data 8080

# On Windows (PowerShell)
.\build\Release\walnut_server.exe .\data 8080
```

Console output:
```
[WALnut] Local   : http://localhost:8080
[WALnut] LAN URL : http://192.168.1.105:8080
[WALnut] Data dir: ./data
[WALnut] Frontend: ready
```
Open **`http://localhost:8080`** in your web browser.

---

### 2. Development Mode (Dual Process with Vite HMR)
For rapid frontend modification with hot-module reloading:

```bash
# Terminal 1: Run the backend engine
./build/walnut_server ./data 8080

# Terminal 2: Run the Vite development server
cd dashboard
npm run dev
```
Open **`http://localhost:5173`**. Vite automatically proxies backend requests to `:8080`.

---

### 3. Standalone Storage & Buffer Pool Demo CLI
```bash
# Linux / macOS
./build/walnut_demo

# Windows
.\build\Release\walnut_demo.exe
```

---

### 4. Resetting the Database
To reset the engine to a clean initial state:
- **Via UI**: Navigate to **Database Admin** (`/admin`) and select **Factory Reset**.
- **Via Filesystem**: Stop `walnut_server` and remove the database files:
  ```bash
  # Linux / macOS
  rm -f data/walnut.db data/walnut.wal data/walnut.meta

  # Windows (PowerShell)
  Remove-Item -Force data\walnut.db, data\walnut.wal, data\walnut.meta
  ```

---

## Offline Wi-Fi Hotspot Demo Setup (Zero Internet)

WALnut is engineered for live classroom and conference demonstrations where **no internet connection is available**:

1. **Activate Host Hotspot**:
   - **Windows**: Settings $\to$ Network & Internet $\to$ Mobile Hotspot $\to$ Turn **On**.
   - **macOS**: System Settings $\to$ General $\to$ Sharing $\to$ Enable **Internet Sharing**.
   - **Linux**: `nmcli device wifi hotspot ssid WALnut-Auction password "walnutdemo2026"`
2. **Start the Engine**:
   ```bash
   ./build/walnut_server ./data 8080
   ```
   *The server binds to `0.0.0.0:8080` and detects the host machine's LAN IP address.*
3. **Audience Connection**:
   - Host opens the **Host Console** (`/host`) on their projector or screen.
   - The **Audience Mobile Pairing** card displays a QR code linking to `http://<HOST_LAN_IP>:8080`.
   - Attendees scan the QR code with their mobile cameras to immediately load the bidding interface.
   - Attendees sign up with a username and bid in real time with sub-millisecond local network latency.

---

## Testing & Verification Suite

WALnut integrates GoogleTest with CTest across all engine components:

```bash
# Linux / macOS
cd build && ctest --output-on-failure

# Windows
cd build ; ctest -C Release --output-on-failure
```

Or run the test binary directly:
```bash
./build/walnut_tests
```

### Test Catalog

| Subsystem | Test File | Key Invariants Verified |
|---|---|---|
| **Slotted Pages** | `tests/storage/test_page.cpp` | Page header binary layout, slot offsets, CRC32 checksum corruption detection. |
| **File Manager** | `tests/storage/test_file_manager.cpp` | Multi-page allocation, page reading/writing, file size boundaries. |
| **Storage Manager** | `tests/storage/test_storage_manager.cpp` | User, Item, and Bid record insertion, packing, and lookup. |
| **Frame** | `tests/buffer/test_frame.cpp` | Frame pinning, unpinning, and dirty state management. |
| **Buffer Pool** | `tests/buffer/test_buffer_pool.cpp` | Frame caching, Clock eviction policy, dirty page writebacks. |
| **Log Manager** | `tests/wal/test_log_manager.cpp` | Append-only WAL writing, monotonic LSN generation, record deserialization. |
| **Recovery Manager**| `tests/wal/test_recovery_manager.cpp` | ARIES 3-Phase recovery: Analysis, Redo, and Undo with CLR generation. |
| **Checkpointer** | `tests/wal/test_checkpointer.cpp` | Fuzzy checkpoint creation, active transaction list recording. |
| **Hash Index** | `tests/index/test_hash_index.cpp` | Key indexing, bucket splitting, concurrent reader/writer locks. |
| **Transactions** | `tests/txn/test_transaction.cpp` | ACID state transitions, read/write set tracking. |
| **Lock Manager** | `tests/txn/test_lock_manager.cpp` | Shared & Exclusive locks, Strict 2PL release at commit/abort. |
| **Deadlock Detector**| `tests/txn/test_deadlock_detector.cpp` | Wait-for graph construction, cycle detection, victim abort. |
| **Concurrent Storage**| `tests/txn/test_concurrent_storage.cpp` | Multi-threaded simultaneous bids on single and multiple auction lots. |

---

## REST & Server-Sent Events API Reference

All requests and responses use `Content-Type: application/json`.

| Method | Endpoint | Description |
|---|---|---|
| `POST` | `/api/auth/signup` | Register a new user (`BIDDER` or `ADMIN`) |
| `POST` | `/api/auth/signin` | Sign in with username/password; returns session token |
| `GET` | `/api/auth/me` | Fetch active authenticated user profile |
| `POST` | `/api/auth/profile` | Update user display name and profile image |
| `POST` | `/api/auth/change-password` | Change user account password |
| `GET` | `/api/items` | Retrieve list of all auction lots |
| `POST` | `/api/items` | Publish a new auction lot |
| `GET` | `/api/items/:id` | Fetch specific lot details and top bids |
| `POST` | `/api/items/:id/close` | Host or Admin closes an active auction lot |
| `POST` | `/api/items/:id/cancel`| Host or Admin cancels an active auction lot |
| `GET` | `/api/items/category/:cat`| Filter auction lots by category |
| `GET` | `/api/items/won/:user_id` | Retrieve all items won by a specific bidder |
| `GET` | `/api/items/host/:user_id`| Retrieve all items hosted by a specific user |
| `POST` | `/api/bids` | Place a concurrent bid (Strict 2PL Exclusive Lock protected) |
| `GET` | `/api/bids/:item_id` | Fetch complete historical bid ledger for an item |
| `POST` | `/api/upload` | Upload item photo (JPEG, PNG, WebP, GIF $\le 5$ MB) |
| `GET` | `/api/stream` | Server-Sent Events (SSE) live push stream |
| `GET` | `/api/stats` | Real-time buffer pool and storage page statistics |
| `GET` | `/api/host-info` | Discover server LAN IP and dashboard URL |
| `POST` | `/api/recovery/simulate-live`| Execute live C++ ARIES recovery simulation |
| `POST` | `/api/admin/reset-db` | Reset database (`purge_items` or `factory_reset`) |

---

## Repository Structure

```
WALnut/
├── CMakeLists.txt                 # Primary CMake build configuration
├── README.md                      # Comprehensive project guide (this document)
├── LICENSE                        # MIT License
├── include/walnut/                # Public C++ Header Library
│   ├── httplib.h                  # Embedded cpp-httplib HTTP & SSE header
│   ├── buffer/                    # Frame and BufferPool headers
│   ├── common/                    # Constants, Types, and Utilities
│   ├── index/                     # Extendible Hash Index header
│   ├── storage/                   # Page, Metadata, FileManager, StorageManager
│   ├── txn/                       # Transaction, Lock, Deadlock headers
│   └── wal/                       # LogManager, RecoveryManager, Checkpointer
├── src/                           # C++ Implementation Files
│   ├── main.cpp                   # Standalone storage demo (walnut_demo)
│   ├── buffer/                    # Buffer pool & frame implementation
│   ├── common/                    # Common types and utilities
│   ├── index/                     # Hash index implementation
│   ├── server/                    # REST API & SSE Server (server.cpp)
│   ├── storage/                   # Page, file manager, storage manager
│   ├── txn/                       # Transactions, lock manager, deadlock detector
│   └── wal/                       # WAL log manager, recovery manager, checkpointer
├── tests/                         # GoogleTest Unit & Integration Test Suites
│   ├── buffer/                    # Buffer pool & frame tests
│   ├── index/                     # Hash index tests
│   ├── storage/                   # Slotted page & storage tests
│   ├── txn/                       # Strict 2PL & deadlock tests
│   └── wal/                       # WAL & ARIES recovery tests
├── dashboard/                     # React 19 + TypeScript Frontend Application
│   ├── index.html                 # Frontend HTML entry point
│   ├── package.json               # Dependencies & scripts
│   ├── vite.config.ts             # Vite configuration with proxy to :8080
│   └── src/
│       ├── components/            # UI components, modals, navigation
│       ├── context/               # Session, Stream, and Toast state providers
│       ├── lib/                   # API client and cryptographic utilities
│       ├── pages/                 # AuctionFloor, EngineExplorer, HostConsole, Admin
│       └── types/                 # TypeScript interfaces and schemas
├── data/                          # Database Storage Directory (walnut.db, walnut.wal, uploads/)
└── DOCUMENTATION/                 # Academic Project Documentation & Proposal
    ├── pbl_final_proposal.pdf     # Official Project Proposal
    ├── WALnut_PRD.docx            # Product Requirements Document
    ├── WALnut_Architecture_Design.pdf # Architecture & Subsystem Design
    ├── WALnut_API_Specification.pdf   # REST & SSE API Contract
    ├── WALnut_Test_Plan.pdf       # Test Plan & Verification Matrix
    ├── WALnut_Project_Plan_and_Team_Division.pdf
    └── WALnut_Work_Breakdown.pdf
```

---

## Milestone Progress Summary (Weeks 1–8)

| Week | Milestone Description | Status |
|:---:|---|:---:|
| **1** | Design freeze (schema definitions, slotted page binary layout, API contract) | ☑ Complete |
| **2** | Storage foundation (4KB slotted page format, CRC32 checksums, file manager) | ☑ Complete |
| **3** | Buffer pool manager (Clock replacement) + Extendible hash index | ☑ Complete |
| **4** | End-to-end single-threaded operations (Users, Lots, Bids, Catalog) | ☑ Complete |
| **5** | Concurrency core (Thread synchronization, Strict Two-Phase Locking / S2PL) | ☑ Complete |
| **6** | Deadlock handling (Dynamic Wait-For Graph cycle detection & victim rollback via `reserveBundle`) | ☑ Complete |
| **7** | Crash recovery (Write-Ahead Logging, STEAL/NO-FORCE, 3-Phase ARIES redo/undo) | ☑ Complete |
| **8** | Integration, embedded REST/SSE server, React dashboard, Engine Explorer & live demo | ☑ Complete |

---

## Troubleshooting & FAQ

### 1. `Failed to bind/listen on 0.0.0.0:8080 - is the port already in use?`
Another application is currently using port `8080`.
- **Linux / macOS**: `lsof -i :8080` then `kill -9 <PID>`
- **Windows (PowerShell)**:
  ```powershell
  Get-Process -Id (Get-NetTCPConnection -LocalPort 8080).OwningProcess | Stop-Process -Force
  ```
- Or run `walnut_server` on an alternate port:
  ```bash
  ./build/walnut_server ./data 8081
  ```

### 2. Frontend shows `Frontend: not found - run npm run build in dashboard/`
The C++ server serves `./dashboard/dist/index.html`. Build the dashboard:
```bash
cd dashboard && npm install && npm run build && cd ..
```

### 3. Audience mobile phones cannot open the URL over Hotspot
- Ensure your host machine firewall allows inbound TCP traffic on port `8080`.
  - **Windows**: Add an inbound firewall rule for Port `8080` (TCP).
  - **Linux**: `sudo ufw allow 8080/tcp`.
- Verify the mobile device is connected to the host's Wi-Fi network and not cellular data.

---

## Academic References

1. **C. Mohan, D. Haderle, C. Lindsay, H. Pirahesh, P. Schwarz** — *ARIES: A Transaction Recovery Method Supporting Fine-Granularity Locking and Partial Rollbacks Using Write-Ahead Logging* (ACM Transactions on Database Systems, Vol. 17, No. 1, March 1992, pp. 94–162).
2. **Abraham Silberschatz, Henry F. Korth, S. Sudarshan** — *Database System Concepts* (7th Edition, McGraw-Hill, 2019).
3. **Abraham Silberschatz, Peter B. Galvin, Greg Gagne** — *Operating System Concepts* (10th Edition, Wiley, 2018).
4. **Jim Gray and Andreas Reuter** — *Transaction Processing: Concepts and Techniques* (Morgan Kaufmann, 1993).
5. **SQLite Architecture Documentation** — *Atomic Commit in SQLite & The SQLite Database File Format* (https://sqlite.org).
6. **PostgreSQL Global Development Group** — *Reliability and the Write-Ahead Log (WAL)* (https://postgresql.org/docs).
7. **MySQL Documentation** — *InnoDB Locking and Transaction Model* (https://dev.mysql.com/doc/refman/8.0/en/innodb-locking-transaction-model.html).
