// =============================================================================
// WALnut – Storage / Buffer Module (Vinayak Suyal)
// src/main.cpp
//
// Demo: initialises the storage layer, creates a few users/items/bids,
// reads them back, and prints buffer-pool statistics.
// =============================================================================

#include "walnut/storage/storage_manager.hpp"
#include "walnut/buffer/buffer_pool.hpp"
#include "walnut/common/utils.hpp"
#include <iostream>
#include <iomanip>
#include <filesystem>

int main() {
    std::cout << "=== WALnut Storage/Buffer Demo (Vinayak Suyal) ===\n\n";

    // Cleanup any leftover data from a previous run.
    const std::string data_dir = "./data";
    std::filesystem::remove(data_dir + "/walnut.db");
    std::filesystem::remove(data_dir + "/walnut.meta");

    // -------------------------------------------------------------------------
    // 1. Initialise StorageManager
    // -------------------------------------------------------------------------
    walnut::StorageManager sm(data_dir);
    sm.initialize();
    std::cout << "[1] StorageManager initialised. Pages so far: "
              << sm.pageCount() << "\n";

    // -------------------------------------------------------------------------
    // 2. Create users
    // -------------------------------------------------------------------------
    walnut::User u1{"u1", "Alice"};
    walnut::User u2{"u2", "Bob"};
    walnut::User u3{"u3", "Charlie"};
    sm.createUser(u1);
    sm.createUser(u2);
    sm.createUser(u3);
    std::cout << "[2] Created 3 users. Pages: " << sm.pageCount() << "\n";

    // -------------------------------------------------------------------------
    // 3. Create items
    // -------------------------------------------------------------------------
    walnut::Item laptop{"item_001", "Gaming Laptop", 500.0, "OPEN", 0.0, ""};
    walnut::Item phone {"item_002", "Smartphone",    200.0, "OPEN", 0.0, ""};
    sm.createItem(laptop);
    sm.createItem(phone);
    std::cout << "[3] Created 2 items. Pages: " << sm.pageCount() << "\n";

    // -------------------------------------------------------------------------
    // 4. Place bids
    // -------------------------------------------------------------------------
    walnut::Bid bid1{0, "item_001", "u1", 550.0, ""};
    walnut::Bid bid2{0, "item_001", "u2", 600.0, ""};
    walnut::Bid bid3{0, "item_002", "u3", 220.0, ""};

    uint64_t id1 = sm.insertBid(bid1);
    uint64_t id2 = sm.insertBid(bid2);
    uint64_t id3 = sm.insertBid(bid3);
    std::cout << "[4] Inserted bids: " << id1 << ", " << id2 << ", " << id3 << "\n";

    sm.updateItemAfterBid("item_001", 600.0, "u2");
    sm.updateItemAfterBid("item_002", 220.0, "u3");

    // -------------------------------------------------------------------------
    // 5. Read back
    // -------------------------------------------------------------------------
    auto found_user = sm.findUserById("u1");
    if (found_user) {
        std::cout << "[5] findUserById(u1) -> username=" << found_user->username << "\n";
    }

    auto all_items = sm.getAllItems();
    std::cout << "[5] getAllItems() returned " << all_items.size() << " items:\n";
    for (auto& item : all_items) {
        std::cout << "    " << item.item_id << ": \"" << item.title
                  << "\" status=" << item.status
                  << " current_bid=" << item.current_bid
                  << " winner=" << item.current_winner << "\n";
    }

    auto bids = sm.getBids("item_001");
    std::cout << "[5] getBids(item_001) -> " << bids.size() << " bids\n";
    for (auto& b : bids) {
        std::cout << "    bid_id=" << b.bid_id
                  << " user=" << b.user_id
                  << " amount=" << b.amount << "\n";
    }

    auto highest = sm.getHighestBid("item_001");
    if (highest) {
        std::cout << "[5] getHighestBid(item_001) -> bid_id=" << highest->bid_id
                  << " amount=" << highest->amount
                  << " by=" << highest->user_id << "\n";
    }

    // -------------------------------------------------------------------------
    // 6. Close item
    // -------------------------------------------------------------------------
    sm.closeItem("item_001");
    auto closed_item = sm.getItem("item_001");
    std::cout << "[6] closeItem(item_001) -> status="
              << (closed_item ? closed_item->status : "NOT FOUND") << "\n";

    // -------------------------------------------------------------------------
    // 7. Buffer pool demo
    // -------------------------------------------------------------------------
    walnut::BufferPool bp(4);
    auto& fm = sm.fileManager();

    // Fetch pages 0, 1, 2, 3 (fills pool)
    for (uint32_t pid = 0; pid < std::min(sm.pageCount(), 4u); ++pid) {
        [[maybe_unused]] auto& page = bp.fetchPage(pid, fm);
        bp.unpinPage(pid, false);
    }
    // Fetch page 0 again (should be a hit if pool has room)
    if (sm.pageCount() > 0) {
        [[maybe_unused]] auto& page = bp.fetchPage(0, fm);
        bp.unpinPage(0, false);
    }

    auto stats = bp.getStats();
    std::cout << "\n[7] BufferPool Stats:\n";
    std::cout << "    Hits:            " << stats.hits           << "\n";
    std::cout << "    Misses:          " << stats.misses         << "\n";
    std::cout << "    Evictions:       " << stats.evictions      << "\n";
    std::cout << "    Dirty evictions: " << stats.dirty_evictions << "\n";

    // -------------------------------------------------------------------------
    // 8. Flush & close cleanly
    // -------------------------------------------------------------------------
    bp.flushAllPages(fm);
    sm.sync();
    sm.close();

    std::cout << "\n=== Demo complete. walnut.db and walnut.meta written to ./data/ ===\n";
    return 0;
}
