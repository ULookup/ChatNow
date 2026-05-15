#pragma once

/**
 * content_hash —— "sha256:<64hex>" 格式的内容哈希校验工具
 * ---
 * 客户端在 ApplyUpload 前对文件计算 SHA-256，按 "sha256:<lower-hex>" 形式
 * 提交给服务端用作去重 key + 内容指纹。本工具仅做格式校验与拆分，
 * 不计算哈希。
 */

#include <cstddef>
#include <string>
#include <string_view>

namespace chatnow::content_hash {

inline bool is_hex_lower(std::string_view s) {
    for (char c : s) {
        bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (!ok) return false;
    }
    return true;
}

/* brief: 校验 "sha256:" + 64 位小写 hex 形式 */
inline bool is_valid(std::string_view h) {
    constexpr std::string_view prefix = "sha256:";
    if (h.size() != prefix.size() + 64) return false;
    if (h.substr(0, prefix.size()) != prefix) return false;
    return is_hex_lower(h.substr(prefix.size()));
}

/* brief: 取出 "sha256:" 之后的 64 hex；无效则返回空串 */
inline std::string hex_part(std::string_view h) {
    return is_valid(h) ? std::string(h.substr(7)) : std::string{};
}

/* brief: 取 hex 前 n 字节作为 object key 子目录前缀（默认 2） */
inline std::string short_prefix(std::string_view h, std::size_t n = 2) {
    auto hex = hex_part(h);
    return hex.size() >= n ? hex.substr(0, n) : std::string{};
}

}  // namespace chatnow::content_hash
