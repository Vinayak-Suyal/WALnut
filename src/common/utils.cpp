// =============================================================================
// WALnut – Storage / Buffer Module (Vinayak Suyal)
// src/common/utils.cpp
// =============================================================================

#include "walnut/common/utils.hpp"
#include "walnut/common/constants.hpp"
#include <ctime>
#include <cstring>
#include <sstream>
#include <iomanip>

namespace walnut::utils {

// ---------------------------------------------------------------------------
std::string currentUtcTimestamp() {
    std::time_t now = std::time(nullptr);
    std::tm     tm_utc{};

#if defined(_WIN32)
    gmtime_s(&tm_utc, &now);
#else
    gmtime_r(&now, &tm_utc);
#endif

    char buf[MAX_TIMESTAMP_LEN];
    // Format: 2026-01-02T15:04:05Z  (20 chars + NUL, well within 32)
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
    return std::string(buf);
}

// ---------------------------------------------------------------------------
bool safeCopyString(const std::string& src, char* dst, std::size_t max_len) {
    if (max_len == 0) return false;

    std::size_t copy_len = src.size();
    bool truncated = false;

    if (copy_len >= max_len) {
        copy_len  = max_len - 1;
        truncated = true;
    }

    std::memcpy(dst, src.data(), copy_len);
    dst[copy_len] = '\0';

    // Zero remaining bytes to avoid leaking old data.
    if (copy_len + 1 < max_len) {
        std::memset(dst + copy_len + 1, 0, max_len - copy_len - 1);
    }

    return !truncated;
}

// ---------------------------------------------------------------------------
std::string readCString(const char* buf, std::size_t max_len) {
    if (!buf || max_len == 0) return {};
    // strnlen for safety: find NUL within max_len bytes.
    std::size_t len = 0;
    while (len < max_len && buf[len] != '\0') ++len;
    return std::string(buf, len);
}

// ---------------------------------------------------------------------------
// Simple CRC-32 like checksum using standard polynomial 0xEDB88320.
// This provides good bit distribution for detecting single-bit errors and
// accidental corruption of pages.
uint32_t computeChecksum(const uint8_t* data, std::size_t length,
                         uint32_t seed) {
    uint32_t crc = seed ^ 0xFFFFFFFFu;
    for (std::size_t i = 0; i < length; ++i) {
        crc ^= static_cast<uint32_t>(data[i]);
        for (int k = 0; k < 8; ++k) {
            if (crc & 1u)
                crc = (crc >> 1) ^ 0xEDB88320u;
            else
                crc >>= 1;
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

} // namespace walnut::utils
