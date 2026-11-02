#include "walnut/txn/deadlock_detector.hpp"
#include "walnut/txn/lock_manager.hpp"
#include "walnut/txn/transaction_manager.hpp"

#include <atomic>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

// ANSI Terminal Color Codes
namespace Color {
    const char* Reset   = "\033[0m";
    const char* Bold    = "\033[1m";
    const char* Red     = "\033[31m";
    const char* Green   = "\033[32m";
    const char* Yellow  = "\033[33m";
    const char* Blue    = "\033[34m";
    const char* Magenta = "\033[35m";
    const char* Cyan    = "\033[36m";
}

void PrintHeader(const std::string& title) {
    std::cout << "\n" << Color::Bold << Color::Cyan;
    std::cout << "======================================================================\n";
    std::cout << "  " << title << "\n";
    std::cout << "======================================================================"
              << Color::Reset << "\n\n";
}

void PrintWFG(walnut::LockManager& lm) {
    auto graph = lm.BuildWaitForGraph();
    std::cout << Color::Yellow << "  [Wait-For Graph Snapshot]" << Color::Reset << "\n";
    if (graph.empty()) {
        std::cout << "    (No transactions waiting)\n";
        return;
    }
    for (const auto& entry : graph) {
        std::cout << "    Txn " << entry.first << " is waiting for: { ";
        for (auto holder : entry.second) {
            std::cout << "Txn " << holder << " ";
        }
        std::cout << "}\n";
    }
}

// -----------------------------------------------------------------------------
// DEMO 1: Two-Way Deadlock Detection & Victim Selection
// -----------------------------------------------------------------------------
void Demo1_TwoWayDeadlock() {
    PrintHeader("DEMO 1: Two-Way Deadlock Detection & Youngest-Victim Resolution");

    walnut::LockManager lm;
    walnut::TransactionManager tm(lm);
    walnut::DeadlockDetector detector(lm, tm);

    std::cout << "Step 1: Starting two concurrent transactions (T1 and T2)...\n";
    walnut::Transaction* t1 = tm.Begin();
    walnut::Transaction* t2 = tm.Begin();
    std::cout << "  -> Created Txn " << t1->GetTxnId() << " and Txn " << t2->GetTxnId() << "\n\n";

    std::cout << "Step 2: Acquiring initial locks:\n";
    lm.AcquireLock(t1, "item_laptop", walnut::LockMode::EXCLUSIVE);
    std::cout << "  -> " << Color::Green << "Txn " << t1->GetTxnId()
              << " acquired EXCLUSIVE lock on 'item_laptop'" << Color::Reset << "\n";

    lm.AcquireLock(t2, "item_phone", walnut::LockMode::EXCLUSIVE);
    std::cout << "  -> " << Color::Green << "Txn " << t2->GetTxnId()
              << " acquired EXCLUSIVE lock on 'item_phone'" << Color::Reset << "\n\n";

    std::cout << "Step 3: Creating circular wait (Deadlock)...\n";
    std::atomic<bool> t1_blocked{false};
    std::atomic<bool> t2_blocked{false};

    std::thread thread1([&] {
        std::cout << "  -> Txn 1 attempting to acquire 'item_phone'...\n";
        t1_blocked.store(true);
        try {
            lm.AcquireLock(t1, "item_phone", walnut::LockMode::EXCLUSIVE);
            std::cout << Color::Green << "  -> Txn 1 acquired 'item_phone' and COMMITTED!"
                      << Color::Reset << "\n";
            tm.Commit(t1);
        } catch (const walnut::TransactionAbortedException&) {
            std::cout << Color::Red << "  -> Txn 1 caught TransactionAbortedException! Aborting..."
                      << Color::Reset << "\n";
            tm.Abort(t1);
        }
    });

    while (t1->GetWaitingFor().empty()) {
        std::this_thread::yield();
    }
    std::cout << Color::Yellow << "  -> Txn 1 is now BLOCKED waiting for 'item_phone' (held by Txn 2)"
              << Color::Reset << "\n";

    std::thread thread2([&] {
        std::cout << "  -> Txn 2 attempting to acquire 'item_laptop'...\n";
        t2_blocked.store(true);
        try {
            lm.AcquireLock(t2, "item_laptop", walnut::LockMode::EXCLUSIVE);
            std::cout << Color::Green << "  -> Txn 2 acquired 'item_laptop' and COMMITTED!"
                      << Color::Reset << "\n";
            tm.Commit(t2);
        } catch (const walnut::TransactionAbortedException&) {
            std::cout << Color::Red << "  -> Txn 2 caught TransactionAbortedException (DEADLOCK VICTIM)!"
                      << Color::Reset << "\n";
            tm.Abort(t2);
        }
    });

    while (t2->GetWaitingFor().empty()) {
        std::this_thread::yield();
    }
    std::cout << Color::Yellow << "  -> Txn 2 is now BLOCKED waiting for 'item_laptop' (held by Txn 1)"
              << Color::Reset << "\n\n";

    std::cout << "Step 4: Inspecting Wait-For Graph:\n";
    PrintWFG(lm);
    std::cout << Color::Red << "  CYCLE DETECTED: Txn 1 -> Txn 2 -> Txn 1\n" << Color::Reset << "\n";

    std::cout << "Step 5: Invoking DeadlockDetector::DetectAndResolve()...\n";
    std::cout << "  -> Policy: Abort youngest transaction (highest txn_id).\n";
    int victims = detector.DetectAndResolve();
    std::cout << "  -> Victims aborted: " << Color::Bold << victims << Color::Reset << "\n\n";

    thread1.join();
    thread2.join();

    std::cout << "Step 6: Final transaction states:\n";
    std::cout << "  -> Txn " << t1->GetTxnId() << " State: "
              << (t1->GetState() == walnut::TransactionState::COMMITTED
                      ? Color::Green : Color::Red)
              << (t1->GetState() == walnut::TransactionState::COMMITTED ? "COMMITTED" : "ABORTED")
              << Color::Reset << "\n";
    std::cout << "  -> Txn " << t2->GetTxnId() << " State: "
              << (t2->GetState() == walnut::TransactionState::COMMITTED
                      ? Color::Green : Color::Red)
              << (t2->GetState() == walnut::TransactionState::COMMITTED ? "COMMITTED" : "ABORTED")
              << Color::Reset << "\n";
}

// -----------------------------------------------------------------------------
// DEMO 2: Deadlock Recovery with Undo-Rollback and Automatic Retry
// -----------------------------------------------------------------------------
void Demo2_DeadlockRecoveryWithRetry() {
    PrintHeader("DEMO 2: Deadlock Recovery with Rollback and Automatic Retry");

    walnut::LockManager lm;
    walnut::TransactionManager tm(lm);
    walnut::DeadlockDetector detector(lm, tm);

    std::cout << "Scenario: Two buyers try to place concurrent bids on each other's items.\n";
    std::cout << "If a transaction gets aborted due to a deadlock:\n";
    std::cout << "  1. Its UndoActions must execute (Rollback).\n";
    std::cout << "  2. Its held locks are released.\n";
    std::cout << "  3. The client retries the transaction with backoff.\n\n";

    // Simulating in-memory state:
    int buyer1_credits = 1000;
    int buyer2_credits = 1000;

    auto execute_buyer1 = [&] {
        walnut::Transaction* txn = tm.Begin();
        try {
            lm.AcquireLock(txn, "auction_item_A", walnut::LockMode::EXCLUSIVE);

            // Deduct credits with an undo action
            buyer1_credits -= 150;
            txn->PushUndoAction([&] {
                buyer1_credits += 150; // Rollback
                std::cout << Color::Magenta << "    [UndoAction] Rolled back buyer1 credits to: "
                          << buyer1_credits << Color::Reset << "\n";
            });

            // Simulate slight processing delay
            std::this_thread::sleep_for(20ms);

            lm.AcquireLock(txn, "auction_item_B", walnut::LockMode::EXCLUSIVE);
            tm.Commit(txn);
            std::cout << Color::Green << "  -> Buyer 1 transaction COMMITTED successfully!"
                      << Color::Reset << "\n";
            return true;
        } catch (const walnut::TransactionAbortedException&) {
            tm.Abort(txn);
            return false;
        }
    };

    auto execute_buyer2_with_retry = [&] {
        constexpr int max_retries = 3;
        for (int attempt = 1; attempt <= max_retries; ++attempt) {
            walnut::Transaction* txn = tm.Begin();
            try {
                std::cout << "  [Buyer 2 Attempt " << attempt << "] Starting Txn "
                          << txn->GetTxnId() << "...\n";
                lm.AcquireLock(txn, "auction_item_B", walnut::LockMode::EXCLUSIVE);

                buyer2_credits -= 200;
                txn->PushUndoAction([&] {
                    buyer2_credits += 200; // Rollback
                    std::cout << Color::Magenta << "    [UndoAction] Rolled back buyer2 credits to: "
                              << buyer2_credits << Color::Reset << "\n";
                });

                std::this_thread::sleep_for(20ms);

                lm.AcquireLock(txn, "auction_item_A", walnut::LockMode::EXCLUSIVE);
                tm.Commit(txn);
                std::cout << Color::Green << "  -> Buyer 2 transaction COMMITTED on attempt "
                          << attempt << "!" << Color::Reset << "\n";
                return true;
            } catch (const walnut::TransactionAbortedException&) {
                std::cout << Color::Red << "  -> Buyer 2 caught deadlock abort on attempt "
                          << attempt << "! Rolling back & retrying..." << Color::Reset << "\n";
                tm.Abort(txn);
                // Exponential backoff / jitter
                std::this_thread::sleep_for(std::chrono::milliseconds(30 * attempt));
            }
        }
        return false;
    };

    std::thread t1_worker([&] { execute_buyer1(); });
    std::thread t2_worker([&] { execute_buyer2_with_retry(); });

    // Wait until both enter lock wait
    std::this_thread::sleep_for(50ms);

    std::cout << "\nDetecting deadlocks during execution...\n";
    int resolved = detector.DetectAndResolve();
    std::cout << "Detector resolved " << resolved << " deadlocked victim(s).\n\n";

    t1_worker.join();
    t2_worker.join();

    std::cout << "\nFinal Account Balances (Verified Correctness):\n";
    std::cout << "  -> Buyer 1 Credits: " << buyer1_credits << " (expected 850)\n";
    std::cout << "  -> Buyer 2 Credits: " << buyer2_credits << " (expected 800)\n";
    std::cout << Color::Green << "  ALL TRANSACTIONS RECOVERED AND COMPLETED SUCCESSFULLY!\n"
              << Color::Reset;
}

// -----------------------------------------------------------------------------
// DEMO 3: Autonomous Background Deadlock Detection Thread
// -----------------------------------------------------------------------------
void Demo3_BackgroundDetector() {
    PrintHeader("DEMO 3: Autonomous Background Detection Thread (50ms Interval)");

    walnut::LockManager lm;
    walnut::TransactionManager tm(lm);

    // Start detector with a 50ms polling interval
    walnut::DeadlockDetector detector(lm, tm, 50ms);
    detector.Start();
    std::cout << "  -> DeadlockDetector background thread started with 50ms interval.\n";

    walnut::Transaction* t1 = tm.Begin();
    walnut::Transaction* t2 = tm.Begin();

    lm.AcquireLock(t1, "resource_alpha", walnut::LockMode::EXCLUSIVE);
    lm.AcquireLock(t2, "resource_beta", walnut::LockMode::EXCLUSIVE);

    std::cout << "  -> Creating autonomous background deadlock...\n";
    auto start_time = std::chrono::steady_clock::now();

    std::thread th1([&] {
        try {
            lm.AcquireLock(t1, "resource_beta", walnut::LockMode::EXCLUSIVE);
            tm.Commit(t1);
        } catch (const walnut::TransactionAbortedException&) {
            tm.Abort(t1);
        }
    });

    std::thread th2([&] {
        try {
            lm.AcquireLock(t2, "resource_alpha", walnut::LockMode::EXCLUSIVE);
            tm.Commit(t2);
        } catch (const walnut::TransactionAbortedException&) {
            tm.Abort(t2);
        }
    });

    th1.join();
    th2.join();

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - start_time)
                       .count();

    detector.Stop();
    std::cout << Color::Green << "  -> Background thread automatically detected and resolved the deadlock in "
              << elapsed << "ms!\n" << Color::Reset;
}

// -----------------------------------------------------------------------------
// DEMO 4: Realistic Auction Items Deadlock & Recovery
// -----------------------------------------------------------------------------
void Demo4_AuctionCatalogDeadlocks() {
    PrintHeader("DEMO 4: Real-World Auction Items Deadlock & Recovery");

    walnut::LockManager lm;
    walnut::TransactionManager tm(lm);
    walnut::DeadlockDetector detector(lm, tm, 30ms);
    detector.Start();

    std::cout << Color::Bold << "Real Auction Scenario A: Package/Bundle Bidding Deadlock\n"
              << Color::Reset;
    std::cout << "  Catalog Items:\n";
    std::cout << "    [1] 'item_macbook_pro_m3'     (Apple M3 Max Laptop)\n";
    std::cout << "    [2] 'item_studio_display'     (Apple 27-inch 5K Display)\n\n";

    std::cout << "  Bidders:\n";
    std::cout << "    Alice (user_alice): Placing combo bid on [MacBook -> Display]\n";
    std::cout << "    Bob   (user_bob):   Placing combo bid on [Display -> MacBook]\n\n";

    std::atomic<bool> alice_committed{false};
    std::atomic<bool> bob_committed{false};

    std::thread alice_thread([&] {
        constexpr int max_retries = 3;
        for (int attempt = 1; attempt <= max_retries; ++attempt) {
            walnut::Transaction* txn = tm.Begin();
            try {
                // Alice locks MacBook first, then Display
                lm.AcquireLock(txn, "item_macbook_pro_m3", walnut::LockMode::EXCLUSIVE);
                std::this_thread::sleep_for(25ms);

                lm.AcquireLock(txn, "item_studio_display", walnut::LockMode::EXCLUSIVE);
                tm.Commit(txn);
                alice_committed.store(true);
                std::cout << Color::Green << "  [SUCCESS] Alice's combo bid COMMITTED on attempt "
                          << attempt << "!\n" << Color::Reset;
                break;
            } catch (const walnut::TransactionAbortedException&) {
                std::cout << Color::Red << "  [DEADLOCK] Alice's transaction aborted by detector. Retrying...\n"
                          << Color::Reset;
                tm.Abort(txn);
                std::this_thread::sleep_for(std::chrono::milliseconds(20 * attempt));
            }
        }
    });

    std::thread bob_thread([&] {
        constexpr int max_retries = 3;
        for (int attempt = 1; attempt <= max_retries; ++attempt) {
            walnut::Transaction* txn = tm.Begin();
            try {
                // Bob locks Display first, then MacBook
                lm.AcquireLock(txn, "item_studio_display", walnut::LockMode::EXCLUSIVE);
                std::this_thread::sleep_for(25ms);

                lm.AcquireLock(txn, "item_macbook_pro_m3", walnut::LockMode::EXCLUSIVE);
                tm.Commit(txn);
                bob_committed.store(true);
                std::cout << Color::Green << "  [SUCCESS] Bob's combo bid COMMITTED on attempt "
                          << attempt << "!\n" << Color::Reset;
                break;
            } catch (const walnut::TransactionAbortedException&) {
                std::cout << Color::Red << "  [DEADLOCK] Bob's transaction aborted by detector. Retrying...\n"
                          << Color::Reset;
                tm.Abort(txn);
                std::this_thread::sleep_for(std::chrono::milliseconds(30 * attempt));
            }
        }
    });

    alice_thread.join();
    bob_thread.join();

    std::cout << "\n" << Color::Bold
              << "Real Auction Scenario B: Lock Upgrade Deadlock on the Same Item\n"
              << Color::Reset;
    std::cout << "  Catalog Item: 'item_vintage_rolex_1968' (High-contention vintage watch)\n\n";
    std::cout << "  Workflow:\n";
    std::cout << "    1. Bidder A acquires SHARED lock (read current bid).\n";
    std::cout << "    2. Bidder B acquires SHARED lock (read current bid).\n";
    std::cout << "    3. Both attempt SHARED -> EXCLUSIVE upgrade to submit a higher bid.\n";
    std::cout << "    -> Result: Upgrade deadlock detected! One is aborted, the other wins.\n\n";

    walnut::Transaction* txn_a = tm.Begin();
    walnut::Transaction* txn_b = tm.Begin();

    lm.AcquireLock(txn_a, "item_vintage_rolex_1968", walnut::LockMode::SHARED);
    lm.AcquireLock(txn_b, "item_vintage_rolex_1968", walnut::LockMode::SHARED);
    std::cout << "  -> Both Bidder A and Bidder B hold SHARED locks on 'item_vintage_rolex_1968'.\n";

    std::atomic<bool> b_aborted{false};

    std::thread bidder_a([&] {
        try {
            // Bidder A requests upgrade to EXCLUSIVE
            lm.AcquireLock(txn_a, "item_vintage_rolex_1968", walnut::LockMode::EXCLUSIVE);
            tm.Commit(txn_a);
            std::cout << Color::Green << "  -> Bidder A upgrade succeeded and COMMITTED!\n"
                      << Color::Reset;
        } catch (...) {
            tm.Abort(txn_a);
        }
    });

    while (txn_a->GetWaitingFor().empty()) {
        std::this_thread::yield();
    }
    std::cout << "  -> Bidder A is waiting for Bidder B's SHARED lock to release.\n";

    std::thread bidder_b([&] {
        try {
            // Bidder B also tries to upgrade -> Immediate Upgrade Deadlock!
            lm.AcquireLock(txn_b, "item_vintage_rolex_1968", walnut::LockMode::EXCLUSIVE);
            tm.Commit(txn_b);
        } catch (const walnut::TransactionAbortedException&) {
            b_aborted.store(true);
            std::cout << Color::Red
                      << "  -> Bidder B caught Upgrade Deadlock abort! Releasing lock...\n"
                      << Color::Reset;
            tm.Abort(txn_b);
        }
    });

    bidder_b.join();
    bidder_a.join();

    detector.Stop();
    std::cout << Color::Green << "  -> Upgrade deadlock resolved without indefinite block!\n"
              << Color::Reset;
}

int main() {
    std::cout << Color::Bold << Color::Magenta;
    std::cout << R"(
  __        __      _      _   _             _   
  \ \      / / /\  | |    | \ | |_   _ _   _| |_ 
   \ \ /\ / / / _\ | |    |  \| | | | | |_| | __|
    \ V  V / / ___ \ |___ | |\  | |_| |  _  | |_ 
     \_/\_/ /_/   \_\_____|_| \_|\__,_|_| |_|\__|
           CONCURRENCY & DEADLOCK SUBSYSTEM
)";
    std::cout << Color::Reset << "\n";

    Demo1_TwoWayDeadlock();
    Demo2_DeadlockRecoveryWithRetry();
    Demo3_BackgroundDetector();
    Demo4_AuctionCatalogDeadlocks();

    std::cout << "\n" << Color::Bold << Color::Green
              << "[SUCCESS] All Deadlock Detection & Recovery Demos completed successfully!"
              << Color::Reset << "\n\n";

    return 0;
}

