#pragma once

/**
 * log_json —— 自构 JSON 一行（不引第三方 JSON 库）
 * ---
 * 输出形如：
 *   {"ts":"2026-05-14T10:23:45.123Z","level":"info","service":"message",
 *    "trace_id":"...","user_id":"...","device_id":"...",
 *    "msg":"the message","fields":{"k":"v"}}
 * ---
 * 设计：
 *   - escape_json：仅处理必须转义字符（"、\、控制字符 < 0x20）；
 *     非 ASCII 原样保留（UTF-8 字节序列在 JSON 中合法）
 *   - build_log_line：把 ts/level/service/LogContext/msg/fields 拼接为单行 JSON
 *   - fields 类型：std::initializer_list<std::pair<std::string_view, std::string>>
 *     调用方需把数值/对象预先 to_string，避免泛型转 JSON 的复杂度
 *   - 时间戳：UTC ISO-8601 毫秒精度（与 spec §5.2 示例一致）
 */

#include "log/log_context.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>

namespace chatnow::infra {

/* brief: 写出 JSON-safe 字符串（不含外侧引号）；高频路径，预 reserve */
inline void escape_json_into(std::string& out, std::string_view s) {
    out.reserve(out.size() + s.size() + 2);
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
}

inline std::string escape_json(std::string_view s) {
    std::string out;
    escape_json_into(out, s);
    return out;
}

/* brief: 当前 UTC 毫秒 ISO-8601，例 "2026-05-14T10:23:45.123Z" */
inline std::string iso8601_utc_now_ms() {
    auto now = std::chrono::system_clock::now();
    auto ms_total = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now.time_since_epoch()).count();
    std::time_t secs = ms_total / 1000;
    int ms = static_cast<int>(ms_total % 1000);
    std::tm tm_utc{};
#if defined(_WIN32)
    gmtime_s(&tm_utc, &secs);
#else
    gmtime_r(&secs, &tm_utc);
#endif
    char buf[32];
    std::snprintf(buf, sizeof(buf),
                  "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                  tm_utc.tm_year + 1900, tm_utc.tm_mon + 1, tm_utc.tm_mday,
                  tm_utc.tm_hour, tm_utc.tm_min, tm_utc.tm_sec, ms);
    return buf;
}

/* brief: 全局服务名（init 阶段调用 set_service_name 设定） */
inline std::string& mutable_service_name() {
    static std::string name = "unknown";
    return name;
}

inline void set_service_name(std::string name) {
    mutable_service_name() = std::move(name);
}

inline const std::string& service_name() {
    return mutable_service_name();
}

using FieldList = std::initializer_list<std::pair<std::string_view, std::string>>;

/* brief: 拼一行 JSON 日志
 *  level 推荐传 "trace"/"debug"/"info"/"warn"/"error"/"fatal" 全小写
 */
inline std::string build_log_line(std::string_view level,
                                  std::string_view msg,
                                  FieldList fields = {})
{
    const auto& ctx = ::chatnow::log::LogContext::current();
    std::string out;
    out.reserve(256 + msg.size());
    out += "{\"ts\":\"";
    out += iso8601_utc_now_ms();
    out += "\",\"level\":\"";
    escape_json_into(out, level);
    out += "\",\"service\":\"";
    escape_json_into(out, service_name());
    out += "\",\"trace_id\":\"";
    escape_json_into(out, ctx.trace_id);
    out += "\",\"user_id\":\"";
    escape_json_into(out, ctx.user_id);
    out += "\",\"device_id\":\"";
    escape_json_into(out, ctx.device_id);
    out += "\",\"msg\":\"";
    escape_json_into(out, msg);
    out += "\",\"fields\":{";
    bool first = true;
    for (const auto& kv : fields) {
        if (!first) out += ",";
        first = false;
        out += "\"";
        escape_json_into(out, kv.first);
        out += "\":\"";
        escape_json_into(out, kv.second);
        out += "\"";
    }
    out += "}}";
    return out;
}

}  // namespace chatnow::infra
