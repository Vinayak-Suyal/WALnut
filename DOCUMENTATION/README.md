# WALnut

**A Concurrent Auction Engine with Deadlock Detection & Recovery**
`OSDBMS-V-2026-T038` · Team **The Acid Mechanics** (T038)

WALnut is a small, from-scratch C++17 storage and transaction engine, built around one
fixed, observable workload — a live auction — to demonstrate Strict Two-Phase Locking,
wait-for-graph deadlock detection, and Write-Ahead-Log crash recovery end to end.

| | |
|---|---|
| Team Lead | Vinayak Suyal — storage manager, buffer pool |
| Member | Sushmit Singh Rawat — transaction & lock manager, deadlock detector |
| Member | Kanak Rawat — WAL, recovery, hash index, test workloads |
| Member | Suhavi Jugran — fixed operation API, REST server, dashboard |

## Documentation

This README covers build/run only. Full specs live alongside it:

- [`WALnut_PRD.docx`](./WALnut_PRD.docx) — requirements, scope, success metrics
- [`WALnut_Architecture_Design.docx`](./WALnut_Architecture_Design.docx) — component design, storage/lock/deadlock/WAL internals
- [`WALnut_API_Specification.docx`](./WALnut_API_Specification.docx) — REST + SSE contract
- [`WALnut_Test_Plan.docx`](./WALnut_Test_Plan.docx) — test cases and milestone acceptance gates

## Tech Stack

- **Engine**: C++17, CMake, POSIX threads (pthreads)
- **API**: embedded REST server (cpp-httplib) in the same process — HTTP/JSON + Server-Sent Events
- **Dashboard**: React + TypeScript + Vite
- **Tests**: GoogleTest / Catch2

## Prerequisites

- CMake ≥ 3.16 and a C++17 compiler (g++ ≥ 9 or clang ≥ 10)
- Node.js ≥ 18 and npm (for the dashboard)
- Linux or macOS (no distributed/multi-machine setup required — see PRD §4.4)

## Build

```bash
# Engine
mkdir -p build && cd build
cmake ..
cmake --build .

# Dashboard
cd ../dashboard
npm install
```

## Run

```bash
# 1. Start the engine (also starts the embedded REST API + SSE stream on :8080)
./build/walnut_engine

# 2. In a second terminal, start the dashboard
cd dashboard
npm run dev
# open the printed localhost URL
```

On first run the engine creates `walnut.db`, `walnut.wal`, and `walnut.meta` in the
working directory. Delete all three to reset to a clean state.

## Running the Live Demo

The dashboard's bidding view is a **real, functional client** — placing a bid there
runs the exact same `placeBid` transaction as a scripted load-test call (PRD FR-3 /
FR-11; API Spec §5). There is no separate "demo mode."

- **🔥 PRIORITY: Offline Hotspot Demo (Assigned to Suhavi Jugran)**: For the live presentation, the engine should run on a local Wi-Fi hotspot broadcasted by the host machine. 
  - The C++ API Server must bind to `0.0.0.0` to accept LAN connections.
  - The dashboard will display a QR code containing the Wi-Fi credentials for attendees to scan and join.
  - A second QR code (or captive portal) will link to the host's LAN IP (`http://<LAN_IP>:8080`) so attendees can bid from their own phones without an internet connection.
- **User Authentication (Signup/Signin)**: the dashboard features a Signup/Signin gateway. Users connecting via the hotspot will be prompted to enter a username to sign up or log in before they can place bids. This replaces the initial fixed roster of demo users, providing a more realistic auction experience.
- **Deadlock demo**: run `scripts/reserve_bundle_demo.sh` (or trigger it from the
  dashboard's "Run Deadlock Demo" button, if wired up) to fire two overlapping
  `reserveBundle` calls on the same two items in mixed order. Watch the wait-for graph
  panel show the cycle form and resolve in real time (Architecture Doc §9.3).
- **Crash recovery demo**: `scripts/crash_recovery_demo.sh` starts the engine, places a
  few bids, kills the process at a controlled point, and restarts it — confirming
  committed bids survive and in-flight ones are undone (Architecture Doc §10.4).

## Running Tests

```bash
cd build
ctest --output-on-failure
```

Test cases are catalogued in `WALnut_Test_Plan.docx`; IDs in test output (e.g.
`TC-LCK-01`, `TC-DLK-02`) map directly to that document's sections 6–13.

## Project Structure (indicative)

```
walnut/
├── src/
│   ├── storage/       # page format, file manager, allocation
│   ├── buffer/         # buffer pool, replacement policy
│   ├── txn/             # transaction manager, lock manager
│   ├── deadlock/     # wait-for graph, cycle detection
│   ├── recovery/     # WAL, redo/undo, checkpointing
│   ├── api/               # fixed operation API + REST/SSE server
│   └── main.cpp
├── tests/               # GoogleTest/Catch2 suites, one per subsystem
├── scripts/             # demo & load-test scripts
├── dashboard/         # React + TypeScript client
└── docs/                  # PRD, Architecture, API Spec, Test Plan (this folder)
```

## Milestone Status

| Week | Milestone | Status |
|---|---|---|
| 1 | Design freeze (schema, ER diagram, API, architecture) | ☐ |
| 2 | Storage foundation (page format, file manager) | ☐ |
| 3 | Buffer pool + hash index | ☐ |
| 4 | Single-threaded operations wired end-to-end | ☐ |
| 5 | Concurrency core — MVP (thread pool, Strict 2PL) | ☐ |
| 6 | Deadlock handling (wait-for graph, victim rollback) | ☐ |
| 7 | Recovery (WAL, fault-injection tests) | ☐ |
| 8 | Integration, dashboard, benchmarks, docs, demo | ☐ |

*(Update the checkboxes as each week's PRD §13 acceptance gate is met.)*

## References

See `WALnut_PRD.docx` §14 for the full reference list (SQLite, PostgreSQL, MySQL/InnoDB
documentation; Silberschatz et al.).
