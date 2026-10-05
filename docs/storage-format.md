# Storage Format Specification

**WALnut – Storage / Buffer Module**
**Author:** Vinayak Suyal (Team Lead, The Acid Mechanics — T038)
**Version:** 1.0 | **Date:** 2026-10

---

## 1. Overview

WALnut uses a **fixed-size page** model. All I/O is performed in units of one page
(4 096 bytes). Every page begins with a fixed 64-byte header followed by a
contiguous data region. Records within the data region are fixed-size and
packed without gaps.

---

## 2. File Layout

| File | Purpose |
|---|---|
| `walnut.db` | Main data file; sequence of fixed-size pages |
| `walnut.meta` | Metadata: database header + allocation bitmap |

### 2.1 `walnut.db`

```
offset 0             : Page 0 (PAGE_SIZE = 4096 bytes)
offset 4096          : Page 1
offset 4096 * n      : Page n
```

The file grows by one page each time a new page is allocated. No holes exist
in this version; page IDs are strictly sequential.

### 2.2 `walnut.meta`

```
offset  0 : uint32_t  magic               (0x57414C4E = 'WALN')
offset  4 : uint32_t  format_version      (1)
offset  8 : uint32_t  next_page_id        – next page to allocate
offset 12 : uint32_t  next_bid_id         – next bid ID (monotonic counter)
offset 16 : uint32_t  checkpoint_lsn      – reserved for WAL (Kanak Rawat)
offset 20 : uint8_t   clean_shutdown      – 1 = clean, 0 = dirty
offset 21 : uint8_t   padding[3]
offset 24 : uint32_t  bitmap_byte_count   – bytes of allocation bitmap that follow
offset 28 : uint8_t   padding[4]
offset 32 : uint8_t[] allocation_bitmap   – bitmap_byte_count bytes; bit i = page i allocated
```

On startup, if `clean_shutdown == 0` (false), a warning is printed and control
is passed to the WAL recovery layer (Kanak Rawat's responsibility).

---

## 3. Page Format

### 3.1 Constants

| Constant | Value | Notes |
|---|---|---|
| `PAGE_SIZE` | 4096 bytes | Configurable in `constants.hpp` |
| `PAGE_HEADER_SIZE` | 64 bytes | Fixed; never changes |
| `PAGE_DATA_SIZE` | 4032 bytes | `PAGE_SIZE − PAGE_HEADER_SIZE` |

### 3.2 `PageHeader` Layout (64 bytes, little-endian)

| Offset | Width | Field | Description |
|---|---|---|---|
| 0 | 4 | `page_id` | Unique 0-indexed page number |
| 4 | 4 | `page_type` | See §3.3 |
| 8 | 4 | `record_count` | Number of records currently stored |
| 12 | 4 | `free_space_offset` | Byte offset from page start of next free byte |
| 16 | 4 | `checksum` | CRC-32 over data bytes `[64..4095]` |
| 20 | 12 | `reserved[3]` | Zeroed; reserved for future fields |
| 32 | 32 | `padding` | Zeroed; fills to PAGE_HEADER_SIZE = 64 |

The checksum covers **only** the data region `[PAGE_HEADER_SIZE .. PAGE_SIZE)`.
The header (including the stored checksum field itself) is excluded, so the
checksum can be stored into the header without self-referencing issues.

### 3.3 Page Types

| Value | Constant | Contents |
|---|---|---|
| 0 | `PAGE_TYPE_INVALID` | Uninitialised / never written |
| 1 | `PAGE_TYPE_USERS` | User records |
| 2 | `PAGE_TYPE_ITEMS` | Item records |
| 3 | `PAGE_TYPE_BIDS` | Bid records (append-only) |
| 4 | `PAGE_TYPE_METADATA` | Reserved |
| 5 | `PAGE_TYPE_FREE` | Reserved |

---

## 4. Record Layout

All records are **fixed-size**. String fields are NUL-terminated within a fixed
buffer. Numeric fields use native little-endian representation (the target
platforms are all little-endian; a future porting note would be to add endian
conversion helpers).

### 4.1 String Field Size Limits

| Field | Buffer size | Maximum user-visible length |
|---|---|---|
| `user_id` | 64 bytes | 63 characters |
| `item_id` | 64 bytes | 63 characters |
| `username` | 128 bytes | 127 characters |
| `title` | 128 bytes | 127 characters |
| `status` | 8 bytes | 7 characters (`"OPEN"` or `"CLOSED"`) |
| `current_winner` | 64 bytes | 63 characters |
| `bid_time` | 32 bytes | 31 characters (ISO-8601: `YYYY-MM-DDTHH:MM:SSZ` = 20 chars) |

Exceeding these limits at the API layer raises `std::invalid_argument`.

### 4.2 User Record (`USER_RECORD_SIZE` = 192 bytes)

```
offset   0 : char[64]  user_id   (NUL-terminated)
offset  64 : char[128] username  (NUL-terminated)
```

### 4.3 Item Record (`ITEM_RECORD_SIZE` = 280 bytes)

```
offset   0 : char[64]   item_id        (NUL-terminated)
offset  64 : char[128]  title          (NUL-terminated)
offset 192 : double     starting_price (8 bytes)
offset 200 : char[8]    status         ("OPEN\0\0\0\0" or "CLOSED\0\0")
offset 208 : double     current_bid    (8 bytes)
offset 216 : char[64]   current_winner (NUL-terminated; empty string if no winner)
```

### 4.4 Bid Record (`BID_RECORD_SIZE` = 176 bytes)

```
offset   0 : uint64_t  bid_id   (8 bytes; monotonically increasing)
offset   8 : char[64]  item_id  (NUL-terminated)
offset  72 : char[64]  user_id  (NUL-terminated)
offset 136 : double    amount   (8 bytes)
offset 144 : char[32]  bid_time (NUL-terminated; ISO-8601 UTC)
```

---

## 5. Records Per Page (Theoretical Maximums)

| Record type | Record size | Max records / page |
|---|---|---|
| User | 192 bytes | 21 (4032 / 192 = 21) |
| Item | 280 bytes | 14 (4032 / 280 = 14) |
| Bid | 176 bytes | 22 (4032 / 176 = 22) |

---

## 6. Allocation Strategy

- **One record type per page.** A USERS page never contains Item records.
- **Append-only within a page.** `free_space_offset` advances forward; deleted records are **not** reclaimed in this version (that is left for a future compaction pass).
- **Current-page tracking.** `StorageManager` keeps one `current_*_page_` pointer per table. When the current page has insufficient room for the next record, `FileManager::allocatePage()` is called to obtain a fresh page, and the pointer is updated.
- **Index rebuild on restart.** On `initialize()`, `StorageManager` scans all pages to rebuild its in-memory `user_index_`, `item_index_`, and `bid_index_` maps. This is O(total pages × records per page) and is acceptable for this milestone.

---

## 7. Checksum Algorithm

```
CRC-32 with polynomial 0xEDB88320 (IEEE 802.3)
Seed: 0xDEADBEEF XOR 0xFFFFFFFF
Input: page bytes [PAGE_HEADER_SIZE .. PAGE_SIZE)
```

The stored value in `PageHeader::checksum` is compared against a freshly
computed checksum on every `FileManager::readPage()` call. A mismatch raises
`std::runtime_error`.

---

## 8. Known Limitations (v1.0)

1. No record deletion / free-space reclamation (append-only).
2. `getAllItems()` and `getBids()` rebuild from the in-memory index (fast), but
   the index scan on startup is linear over all pages.
3. String fields use a fixed maximum – very long usernames / titles will be
   rejected.
4. No endian conversion; little-endian hosts only.
5. `walnut.db` grows monotonically; no file shrinking.
