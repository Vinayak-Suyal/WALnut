# Buffer Pool Design

**WALnut – Storage / Buffer Module**
**Author:** Vinayak Suyal (Team Lead, The Acid Mechanics — T038)
**Version:** 1.0 | **Date:** 2026-10

---

## 1. Overview

The `BufferPool` is a fixed-size in-memory cache of disk pages. It sits between
the `StorageManager` (high-level CRUD) and the `FileManager` (raw page I/O).
Its purpose is to:

1. Reduce disk I/O by caching frequently accessed pages.
2. Provide a single point of memory-to-disk coordination.
3. Offer a clean integration point for the WAL layer (Write-Ahead Logging).

---

## 2. Frame Structure

A `Frame` is one slot in the buffer pool. Each frame contains:

| Field | Type | Description |
|---|---|---|
| `page_` | `Page` | The 4 KB page currently resident in this frame |
| `pin_count_` | `int` | Number of threads holding this page |
| `valid_` | `bool` | True if this frame contains a loaded page |
| `dirty_` | `bool` | True if this frame has been modified since last flush |
| `reference_bit_` | `bool` | Used by Clock replacement (see §4) |

### Frame Lifecycle

```
[empty] --loadPage()--> [valid, pinned=0, dirty=false, ref=true]
                                  |
              pin() ↓             | unpin()
        [pin_count > 0]   [pin_count == 0, eligible for eviction]
                                  |
                    reset() --------> [empty]
```

---

## 3. Pin / Unpin Semantics

**Pin** (`pin()`) signals that a caller is actively using the page. A pinned
frame is **never evicted** by the Clock algorithm. The `fetchPage()` operation
automatically pins the returned page.

**Unpin** (`unpinPage(page_id, dirty)`) signals that the caller is done with
the page. If `dirty = true`, the frame is marked dirty and will be flushed to
disk before or during eviction.

**Contract for callers (e.g., Sushmit's transaction layer):**

```
Page& page = pool.fetchPage(pid, fm);  // pin_count++
// ... read/modify page ...
pool.unpinPage(pid, was_modified);     // pin_count--
```

A page must **never** be held across a thread boundary without an explicit
`pinPage()` call. The reference returned by `fetchPage()` is only valid while
the frame is pinned.

---

## 4. Clock Replacement Algorithm

The Clock (Second Chance) algorithm approximates LRU with O(1) amortised cost.

### State

- `clock_hand_` – index into `frames_` array, wraps around.
- Each frame has a `reference_bit_`.

### Algorithm (on victim selection)

```
Repeat (up to 2 × frame_count sweeps):
    Let f = frames_[clock_hand_]
    If f is not valid:
        Return f immediately (free slot).
    If f.pin_count > 0:
        Advance hand, skip.
    Else if f.reference_bit == true:
        f.reference_bit = false   ← "second chance"
        Advance hand.
    Else:
        Return f as victim.       ← reference_bit == false, unpinned
If no victim found: throw (pool exhausted)
```

The two-sweep limit ensures we always make progress and detect the all-pinned
case in bounded time.

### Why Clock over LRU?

LRU requires updating a sorted structure on every access (O(log n) or O(n)).
Clock achieves an approximation with O(1) per eviction by using a single bit per
frame, which is ideal for a storage engine where eviction is rare compared to
fetches.

---

## 5. Thread-Safety Approach

All public methods of `BufferPool` are protected by a single `std::mutex`.

```
mutex_ acquired
    ├── findFrame() – lookup page_table_
    ├── selectVictim() – walk frames_
    ├── evictFrame() – may call FileManager::writePage()
    ├── Frame::pin() / Frame::unpin()
    └── Stats update
mutex_ released
```

**Lock-ordering rule:** `BufferPool::mutex_` must never be acquired while
holding any lock from Sushmit's transaction/lock layer. The transaction layer
should acquire its locks **first**, then call buffer pool methods.

**Future improvement:** A readers-writer lock (`std::shared_mutex`) would allow
concurrent reads while serialising writes. This is not yet implemented.

---

## 6. Statistics Tracking

```cpp
struct Stats {
    size_t hits;            // fetchPage() found the page in pool
    size_t misses;          // fetchPage() triggered a disk read
    size_t evictions;       // total frames evicted (dirty + clean)
    size_t dirty_evictions; // evictions that required a disk write
};
```

`getStats()` returns a snapshot under the lock. `resetStats()` zeroes all
counters. These can be exposed via the REST API (Suhavi's layer) or logged
periodically.

---

## 7. WAL Integration Hook (for Kanak Rawat)

The buffer pool provides a `setPreFlushHook` callback:

```cpp
using PreFlushHook = std::function<bool(uint32_t page_id)>;
void setPreFlushHook(PreFlushHook hook);
```

This hook is called immediately before any dirty page is written to disk
(whether from `flushPage()`, `flushAllPages()`, or eviction). The hook returns:

- `true` → proceed with the flush (WAL record for this page has been flushed).
- `false` → skip the flush (WAL not yet flushed – Write-Ahead-Log rule).

**Integration pattern (Kanak's WAL layer):**

```cpp
pool.setPreFlushHook([&wal](uint32_t page_id) -> bool {
    return wal.isPageSafeToFlush(page_id);
});
```

By default (no hook registered) all flushes proceed unconditionally.

---

## 8. Integration with FileManager

```
StorageManager
    ├── FileManager          ← raw page I/O
    │       └── walnut.db
    └── (optional) BufferPool ← caches FileManager pages
```

`fetchPage(page_id, file_manager)` passes the `FileManager` reference so that
any disk read or write during the fetch/eviction can happen inside the pool
operation without the caller needing separate I/O handles.

The `StorageManager` in this milestone does **not** use the `BufferPool` by
default (it calls `FileManager` directly). The buffer pool is available as a
standalone component and can be layered in front of `FileManager` by:

1. Replacing `file_manager_.readPage(pid)` → `pool.fetchPage(pid, file_manager_)`.
2. Replacing `file_manager_.writePage(page)` → `pool.unpinPage(pid, /*dirty=*/true)`.

This layering is deferred to the concurrency integration milestone (Sushmit's
transaction layer passes pages through the pool).

---

## 9. Configuration

| Parameter | Default | Set via |
|---|---|---|
| Frame count | 64 | `BufferPool(frame_count)` constructor |

The default of 64 frames (256 KB of cache) is conservative and appropriate for
tests. Production usage should pass a larger value (e.g., 1024 frames = 4 MB).
