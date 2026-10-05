// =============================================================================
// WALnut – Storage / Buffer Module (Vinayak Suyal)
// include/walnut/common/utils.hpp
//
// Utility helpers: string operations, time utilities used by the storage layer.
// =============================================================================

#pragma once

#include <string>
#include <cstdint>

namespace walnut::utils {

/// Return the current UTC time as an ISO-8601 string (YYYY-MM-DDTHH:MM:SSZ).
/// Used by StorageManager when assigning bid_time.
std::string currentUtcTimestamp();

/// Copy at most max_len-1 bytes of src into dst, always NUL-terminating.
/// Returns false (and truncates) if src.size() >= max_len.
bool safeCopyString(const std::string& src, char* dst, std::size_t max_len);

/// Read a NUL-terminated C string from buf into a std::string.
/// Reads at most max_len bytes.
std::string readCString(const char* buf, std::size_t max_len);

/// Simple CRC32-style checksum over a byte buffer.
uint32_t computeChecksum(const uint8_t* data, std::size_t length,
                         uint32_t seed = 0xDEADBEEF);

} // namespace walnut::utils
