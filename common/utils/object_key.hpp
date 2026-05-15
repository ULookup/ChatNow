#pragma once

/**
 * object_key —— S3/MinIO 对象 Key 生成
 * ---
 * 业务场景按 purpose 选两种 key 风格：
 *   - chat（私密历史消息）：date + hash 双层分片，支持冷热分区
 *       chat/2024/05/14/ab/abcd...   （ab=hash 前 2 hex）
 *   - avatar / sticker / group_avatar（公共可读）：hash 直接寻址
 *       avatar/<64hex>
 *
 * 时间用 UTC，避免本机时区改变后 key 漂移。
 */

#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>
#include <string_view>
#include "utils/content_hash.hpp"

namespace chatnow::object_key {

/* brief: epoch_ms → "YYYY/MM/DD"（UTC） */
inline std::string yyyymmdd_slashed(int64_t epoch_ms) {
    std::time_t t = static_cast<std::time_t>(epoch_ms / 1000);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04d/%02d/%02d",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    return std::string(buf);
}

/* brief: <prefix>/<yyyy/mm/dd>/<hh>/<64hex>，hh 取 hash 前 2 hex */
inline std::string build(std::string_view prefix, std::string_view content_hash, int64_t epoch_ms) {
    auto hex = content_hash::hex_part(content_hash);
    auto sub = hex.substr(0, 2);
    std::string out;
    out.reserve(prefix.size() + 32 + hex.size());
    out.append(prefix).append("/")
       .append(yyyymmdd_slashed(epoch_ms)).append("/")
       .append(sub).append("/")
       .append(hex);
    return out;
}

/* brief: <prefix>/<64hex>（无日期分片） */
inline std::string build_flat(std::string_view prefix, std::string_view content_hash) {
    auto hex = content_hash::hex_part(content_hash);
    std::string out;
    out.reserve(prefix.size() + 1 + hex.size());
    out.append(prefix).append("/").append(hex);
    return out;
}

}  // namespace chatnow::object_key
