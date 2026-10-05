# WALnut

**A Concurrent Auction Engine with Deadlock Detection & Recovery**
`OSDBMS-V-2026-T038` · Team **The Acid Mechanics** (T038)

| Role | Member | Subsystem |
|---|---|---|
| **Team Lead** | **Vinayak Suyal** | **Storage manager, buffer pool, page format ← THIS MODULE** |
| Member | Sushmit Singh Rawat | Transaction manager, lock manager, Strict 2PL, deadlock detector |
| Member | Kanak Rawat | WAL, recovery, hash index, test workloads |
| Member | Suhavi Jugran | Fixed operation API, REST server, dashboard |

> **Scope of this README:** Vinayak's storage/buffer module only.
> The full project README is at [`DOCUMENTATION/README.md`](DOCUMENTATION/README.md).

---

## 1. What Is In This Module

| Path | Contents |
|---|---|
| `include/walnut/common/` | `constants.hpp`, `types.hpp`, `utils.hpp` |
| `include/walnut/storage/` | `page.hpp`, `file_manager.hpp`, `storage_manager.hpp`, `metadata.hpp` |
| `include/walnut/buffer/` | `buffer_pool.hpp`, `frame.hpp` |
| `src/` | Corresponding `.cpp` implementations |
| `tests/storage/` | `test_page`, `test_file_manager`, `test_storage_manager` |
| `tests/buffer/` | `test_frame`, `test_buffer_pool` |
| `docs/storage-format.md` | Page format, record layouts, field size limits |
| `docs/buffer-pool-design.md` | Frame structure, Clock algorithm, thread-safety, WAL hook |
| `data/` | Runtime data directory (`walnut.db`, `walnut.meta` written here) |

**NOT in this module:** transactions, lock manager, deadlock detection, WAL,
recovery, REST API, dashboard, hash index.

---

## 2. Prerequisites

- CMake ≥ 3.16
- C++17 compiler: GCC ≥ 9, Clang ≥ 10, or MSVC 2019+
- Internet access for GoogleTest (fetched automatically by CMake)
- Linux or macOS (primary targets); Windows works with MSVC/MinGW

---

## 3. Build

```bash
# From the repository root:
mkdir -p build
cd build
cmake ..
cmake --build . -j$(nproc)
```

This produces:
- `build/walnut_storage` – static library
- `build/walnut_demo`    – demonstration binary
- `build/walnut_tests`   – GoogleTest binary

---

## 4. Run the Demo

```bash
./build/walnut_demo
```

Expected output (abbreviated):
```
=== WALnut Storage/Buffer Demo (Vinayak Suyal) ===

[1] StorageManager initialised. Pages so far: 0
[2] Created 3 users. Pages: 1
[3] Created 2 items. Pages: 2
[4] Inserted bids: 0, 1, 2
[5] findUserById(u1) -> username=Alice
[5] getAllItems() returned 2 items:
    item_001: "Gaming Laptop" status=OPEN current_bid=600 winner=u2
    item_002: "Smartphone" status=OPEN current_bid=220 winner=u3
[5] getBids(item_001) -> 2 bids
[5] getHighestBid(item_001) -> bid_id=1 amount=600 by=u2
[6] closeItem(item_001) -> status=CLOSED

[7] BufferPool Stats:
    Hits:            1
    Misses:          2
    Evictions:       0
    Dirty evictions: 0

=== Demo complete. walnut.db and walnut.meta written to ./data/ ===
```

---

## 5. Run Tests

```bash
cd build
ctest --output-on-failure
# Or run directly for verbose output:
./walnut_tests
```

### Test IDs

| ID | Suite | Description |
|---|---|---|
| `TC-STO-01` | `test_page`, `test_file_manager` | Page write/read byte-identical round-trip |
| `TC-STO-03` | `test_file_manager`, `test_storage_manager` | New page allocated when current is full |
| `TC-BUF-01` | `test_buffer_pool` | Pinned page is never evicted |
| `TC-BUF-02` | `test_buffer_pool` | Clock selects correct victim |
| `TC-BUF-04` | `test_buffer_pool` | Hit/miss accounting exact |

See `docs/storage-format.md` and `docs/buffer-pool-design.md` for design details.

---

## 6. Example API Usage

### StorageManager

```cpp
#include "walnut/storage/storage_manager.hpp"
using namespace walnut;

StorageManager sm("./data");
sm.initialize();

// Users
sm.createUser({"u1", "Alice"});
auto user = sm.findUserById("u1"); // std::optional<User>

// Items
sm.createItem({"itm1", "Antique Vase", 500.0, "OPEN", 0.0, ""});
sm.updateItemAfterBid("itm1", 600.0, "u1");
sm.closeItem("itm1");

// Bids
uint64_t bid_id = sm.insertBid({0, "itm1", "u1", 600.0, ""});
auto bids = sm.getBids("itm1");
auto highest = sm.getHighestBid("itm1");

sm.close(); // flush + clean shutdown
```

### BufferPool

```cpp
#include "walnut/buffer/buffer_pool.hpp"
using namespace walnut;

BufferPool pool(64);  // 64 frames = 256 KB cache
auto& fm = sm.fileManager();

Page& page = pool.fetchPage(0, fm);  // pin count++
// ... read/write page ...
pool.unpinPage(0, /*dirty=*/true);   // pin count--

pool.flushAllPages(fm);
auto stats = pool.getStats();
// stats.hits, stats.misses, stats.evictions, stats.dirty_evictions

// WAL integration hook (for Kanak):
pool.setPreFlushHook([](uint32_t page_id) -> bool {
    return wal.isPageSafeToFlush(page_id);
});
```

---

## 7. Handoff Interfaces

These match the shared interface sketch from the project plan:

```cpp
// Buffer pool
Page& fetchPage(uint32_t page_id, FileManager& fm);
void  pinPage  (uint32_t page_id);
void  unpinPage(uint32_t page_id, bool dirty);
void  flushPage(uint32_t page_id, FileManager& fm);

// Storage manager
Item                 getItem          (const std::string& item_id);
std::optional<Item>  getItem          (const std::string& item_id) const;
uint64_t             insertBid        (const Bid& bid);
void                 updateItemAfterBid(const std::string& item_id,
                                        double amount,
                                        const std::string& winner);
```

---

## 8. Milestone Status

| Week | Milestone | Status |
|---|---|---|
| 2 | Storage foundation (page format, file manager) | ✅ Done |
| 3 | Buffer pool + Clock replacement | ✅ Done |
| 3 | Thread-safe buffer pool | ✅ Done |
| 3 | Storage + buffer tests | ✅ Done |
| 4 | Wire into single-threaded operations | ⬜ Next |

---

## 9. Next Actions for Other Team Members

### Sushmit Singh Rawat – Transaction / Lock Manager

- Wrap every `StorageManager` public method in a transaction scope.
- Acquire item-level write locks before calling `updateItemAfterBid` / `closeItem`.
- Acquire user-level locks before `createUser`.
- After acquiring locks, call `BufferPool::fetchPage()` to get the in-memory page.
- Use `BufferPool::unpinPage(pid, /*dirty=*/true)` to mark pages modified.
- The `BufferPool::mutex_` is **internal**; do not hold it from outside.

### Kanak Rawat – WAL / Recovery / Hash Index

- Integrate WAL by registering a `setPreFlushHook` on the `BufferPool`:
  ```cpp
  pool.setPreFlushHook([&wal](uint32_t pid) { return wal.isFlushSafe(pid); });
  ```
- On recovery startup, check `FileManager::metadata().clean_shutdown == false`.
  If so, replay WAL before rebuilding indexes.
- `StorageManager::fileManager()` exposes the `FileManager` for direct WAL
  log-record attachment.
- The `checkpoint_lsn` field in `walnut.meta` is reserved for your checkpoint
  pointer.

### Suhavi Jugran – REST API / Dashboard

- **🔥 PRIORITY: Implement the Offline Hotspot Demo:**
  - Ensure the embedded REST server (`cpp-httplib`) binds to `0.0.0.0` (all interfaces) rather than just `127.0.0.1` (localhost).
  - Add a feature to the React dashboard that displays a QR Code containing the local network IP so mobile users can auto-connect to the web interface.
- **🔥 PRIORITY: Signup/Signin Page:**
  - Create a basic Signup/Signin gateway in the React dashboard for users joining the hotspot.
  - Call `sm.createUser()` (via the REST API) when a new user signs up, establishing their `user_id` before routing them to the bidding UI.
- Instantiate `StorageManager` once at engine startup and inject it into your
  HTTP handlers.
- Call `sm.createUser()`, `sm.createItem()`, `sm.insertBid()`,
  `sm.updateItemAfterBid()`, `sm.getItem()`, `sm.getAllItems()`,
  `sm.getBids()`, `sm.getHighestBid()` from your fixed-operation API.
- Buffer pool stats are exposed via `BufferPool::getStats()` – pipe these into
  the dashboard's stats panel.
- Call `sm.sync()` periodically or before exposing a "checkpoint" endpoint.
