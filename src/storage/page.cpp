// =============================================================================
// WALnut – Storage / Buffer Module (Vinayak Suyal)
// src/storage/page.cpp
// =============================================================================

#include "walnut/storage/page.hpp"
#include "walnut/common/utils.hpp"
#include <cstring>
#include <stdexcept>
#include <sstream>

namespace walnut {

// ---------------------------------------------------------------------------
Page::Page() {
    std::memset(buffer_, 0, PAGE_SIZE);
    dirty_ = false;
    // Header: invalid page (type = 0, all zeros)
    header()->page_id           = 0;
    header()->page_type         = PAGE_TYPE_INVALID;
    header()->record_count      = 0;
    header()->free_space_offset = static_cast<uint32_t>(PAGE_HEADER_SIZE);
    header()->checksum          = 0;
    std::memset(header()->reserved, 0, sizeof(header()->reserved));
    std::memset(header()->padding, 0, sizeof(header()->padding));
}

// ---------------------------------------------------------------------------
Page::Page(uint32_t page_id, uint32_t page_type) : Page() {
    header()->page_id   = page_id;
    header()->page_type = page_type;
    header()->free_space_offset = static_cast<uint32_t>(PAGE_HEADER_SIZE);
    updateChecksum();
}

// ---------------------------------------------------------------------------
uint32_t Page::pageId()          const { return header()->page_id; }
uint32_t Page::pageType()        const { return header()->page_type; }
uint32_t Page::recordCount()     const { return header()->record_count; }
uint32_t Page::freeSpaceOffset() const { return header()->free_space_offset; }
uint32_t Page::storedChecksum()  const { return header()->checksum; }

void Page::setPageId(uint32_t id)        { header()->page_id   = id; }
void Page::setPageType(uint32_t type)    { header()->page_type = type; }
void Page::setRecordCount(uint32_t c)    { header()->record_count = c; }
void Page::setFreeSpaceOffset(uint32_t o){ header()->free_space_offset = o; }

void Page::incrementRecordCount() { ++(header()->record_count); }
void Page::decrementRecordCount() {
    if (header()->record_count > 0)
        --(header()->record_count);
}

// ---------------------------------------------------------------------------
// Checksum covers only the data bytes (PAGE_HEADER_SIZE .. PAGE_SIZE).
// The header (including the stored checksum field) is excluded so that the
// checksum can be written into the header without invalidating itself.
// ---------------------------------------------------------------------------
uint32_t Page::computeChecksum() const {
    return utils::computeChecksum(buffer_ + PAGE_HEADER_SIZE,
                                  PAGE_SIZE - PAGE_HEADER_SIZE,
                                  CHECKSUM_SEED);
}

void Page::updateChecksum() {
    header()->checksum = computeChecksum();
}

bool Page::verifyChecksum() const {
    return header()->checksum == computeChecksum();
}

// ---------------------------------------------------------------------------
bool Page::hasRoom(std::size_t record_size) const {
    uint32_t current_offset = header()->free_space_offset;
    return (current_offset + record_size) <= PAGE_SIZE;
}

// ---------------------------------------------------------------------------
void Page::appendRecord(const uint8_t* src, std::size_t record_size) {
    if (!hasRoom(record_size)) {
        throw std::runtime_error(
            "Page::appendRecord – page " +
            std::to_string(pageId()) + " is full");
    }
    uint32_t offset = header()->free_space_offset;
    std::memcpy(buffer_ + offset, src, record_size);
    header()->free_space_offset = static_cast<uint32_t>(offset + record_size);
    ++(header()->record_count);
    dirty_ = true;
}

// ---------------------------------------------------------------------------
const uint8_t* Page::getRecord(uint32_t n, std::size_t record_size) const {
    if (n >= header()->record_count) {
        throw std::out_of_range(
            "Page::getRecord – slot " + std::to_string(n) +
            " out of range (record_count=" +
            std::to_string(header()->record_count) + ")");
    }
    std::size_t byte_offset = PAGE_HEADER_SIZE + n * record_size;
    if (byte_offset + record_size > PAGE_SIZE) {
        throw std::out_of_range("Page::getRecord – computed offset exceeds page boundary");
    }
    return buffer_ + byte_offset;
}

uint8_t* Page::getRecord(uint32_t n, std::size_t record_size) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
    return const_cast<uint8_t*>(
        static_cast<const Page*>(this)->getRecord(n, record_size));
}

} // namespace walnut
