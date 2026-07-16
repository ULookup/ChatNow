#pragma once

#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>

namespace chatnow {

inline constexpr uint64_t kUnknownCacheVersion =
    std::numeric_limits<uint64_t>::max();

inline std::optional<uint64_t> parse_cache_version(const std::string &raw) {
    if (raw.empty()) return std::nullopt;
    uint64_t parsed = 0;
    for (char ch : raw) {
        if (ch < '0' || ch > '9') return std::nullopt;
        auto digit = static_cast<uint64_t>(ch - '0');
        if (parsed > (kUnknownCacheVersion - digit) / 10) {
            return std::nullopt;
        }
        parsed = parsed * 10 + digit;
    }
    if (parsed == kUnknownCacheVersion) return std::nullopt;
    return parsed;
}

inline bool cache_version_is_known(uint64_t version) {
    return version != kUnknownCacheVersion;
}

inline bool cache_version_accepts_warm(uint64_t observed_version,
                                       uint64_t current_version) {
    if (!cache_version_is_known(observed_version) ||
        !cache_version_is_known(current_version)) {
        return false;
    }
    return observed_version == current_version;
}

inline bool cache_snapshot_is_stable(uint64_t before_version,
                                     uint64_t after_version) {
    return cache_version_accepts_warm(before_version, after_version);
}

inline bool cache_l1_version_matches(uint64_t cached_version,
                                     uint64_t current_version) {
    return cache_version_accepts_warm(cached_version, current_version);
}

inline bool cache_sentinel_confirms_empty(bool stable_snapshot_empty,
                                          bool sentinel_exists) {
    return stable_snapshot_empty && sentinel_exists;
}

inline bool cache_ttl_allows_write(std::chrono::seconds ttl) {
    return ttl > std::chrono::seconds::zero();
}

} // namespace chatnow
