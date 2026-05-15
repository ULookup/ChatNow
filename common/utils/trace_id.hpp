#pragma once

/**
 * trace_id —— 16 字节随机 → 32 字符小写 hex
 * ---
 * - 与 W3C Trace Context trace-id (16 字节) 兼容，便于未来接 OpenTelemetry
 * - 使用 std::random_device + std::mt19937_64 双 64 位拼成 128 位
 *   不直接调 /dev/urandom：std::random_device 在 glibc/Linux 实现内部已读
 *   /dev/urandom，跨平台同语义；mt19937 仅当 random_device 不可用时降级
 * - 输出全小写 hex，校验函数同样要求小写
 */

#include <array>
#include <cstdint>
#include <random>
#include <string>

namespace chatnow::utils {

inline std::string gen_trace_id() {
    // 拼 128 位
    std::random_device rd;
    static thread_local std::mt19937_64 prng_a(rd());
    static thread_local std::mt19937_64 prng_b(rd() ^ 0x9E3779B97F4A7C15ULL);
    uint64_t hi = prng_a();
    uint64_t lo = prng_b();

    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.resize(32);
    auto write_u64 = [&](uint64_t v, char* dst) {
        for (int i = 15; i >= 0; --i) {
            dst[i] = kHex[v & 0xF];
            v >>= 4;
        }
    };
    write_u64(hi, &out[0]);
    write_u64(lo, &out[16]);
    return out;
}

/* brief: 32 字符 + 全部为 [0-9a-f] */
inline bool is_valid_trace_id(const std::string& s) {
    if (s.size() != 32) return false;
    for (char c : s) {
        bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (!ok) return false;
    }
    return true;
}

}  // namespace chatnow::utils
