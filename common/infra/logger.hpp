#pragma once

/**
 * ===========================================================================
 * 日志封装（基于 spdlog）—— P8 改造为结构化 JSON 输出
 * ---------------------------------------------------------------------------
 * 设计要点：
 *   1. spdlog pattern 切为 "%v"：sink 仅打印 message 原文，不再叠加自己的
 *      时间戳/level/线程 ID。完整 JSON 由 LOG_xxx 宏侧组装。
 *   2. 宏侧自构 JSON：调用 build_log_line(level, msg, fields)，从 LogContext
 *      读 trace_id / user_id / device_id；服务名由 set_service_name 全局设
 *   3. 字段化变体：LOG_INFOF("event", {{"k","v"}}) —— 适合 spec §5.2 推荐用法
 *      传统宏 LOG_INFO("text {}", arg) 仍然工作，输出 fields 为空对象
 *   4. flush 策略不变：debug 模式即刷；release 模式 warn 以上即刷，余 3s 刷
 *   5. async 写盘 + rotating file 不变
 * ===========================================================================
 */

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/async.h>
#include <spdlog/fmt/fmt.h>
#include <chrono>
#include <memory>
#include <string>

#include "infra/log_json.hpp"   // build_log_line / set_service_name

namespace chatnow
{

inline std::shared_ptr<spdlog::logger> g_default_logger;

inline constexpr size_t kMaxFileSize  = 50 * 1024 * 1024;
inline constexpr size_t kMaxFileCount = 10;
inline constexpr size_t kAsyncQueue   = 8192;

/* brief: 设置当前进程的服务名（用于日志 service 字段）
 *  在 main() 早于 init_logger 调用，例如：
 *    chatnow::set_service_name("message");
 *    chatnow::init_logger(release_mode, "/var/log/chatnow/message.log", level);
 */
using ::chatnow::infra::set_service_name;

inline void init_logger(bool mode, const std::string &filename, int32_t level) {
    if(mode == false) {
        spdlog::drop("console-logger");
        g_default_logger = spdlog::stdout_color_mt("console-logger");
        g_default_logger->set_level(spdlog::level::trace);
        g_default_logger->flush_on(spdlog::level::trace);
    } else {
        spdlog::drop("file-logger");
        spdlog::init_thread_pool(kAsyncQueue, 1);
        auto sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            filename, kMaxFileSize, kMaxFileCount);
        g_default_logger = std::make_shared<spdlog::async_logger>(
            "file-logger",
            sink,
            spdlog::thread_pool(),
            spdlog::async_overflow_policy::block);
        spdlog::register_logger(g_default_logger);
        g_default_logger->set_level(static_cast<spdlog::level::level_enum>(level));
        g_default_logger->flush_on(spdlog::level::warn);
        spdlog::flush_every(std::chrono::seconds(3));
    }
    /* P8: 把 sink 输出格式改为仅打印 message —— 业务侧已经组装好 JSON */
    g_default_logger->set_pattern("%v");
}

/* brief: 抽 basename，用于把文件名加进 fields（不再放在 msg 里）  */
constexpr const char* basename_of(const char* path) {
    const char* last = path;
    for(const char* p = path; *p; ++p) {
        if(*p == '/' || *p == '\\') last = p + 1;
    }
    return last;
}

namespace detail {
/* brief: 把传统 LOG_xxx 的 fmt-style 参数拼成 msg 后交给 build_log_line */
template <typename... Args>
inline std::string format_text(const std::string& fmt_str, Args&&... args) {
    if constexpr (sizeof...(Args) == 0) {
        return fmt_str;
    } else {
        return fmt::format(fmt_str, std::forward<Args>(args)...);
    }
}
}  // namespace detail

}  // namespace chatnow

/* ============================================================
 *  传统宏 —— 输出 JSON，msg 字段为格式化后文本，fields 为空
 *  注意：原宏在 msg 前缀加 "[file:line] "，P8 改为放进 fields
 *  仍保留 file/line 作为 fields，便于排障定位
 * ============================================================ */
#define _CN_LOG_JSON(level_enum, level_str, format, ...)                       \
    do {                                                                       \
        if (!chatnow::g_default_logger) break;                                 \
        std::string _msg = ::chatnow::detail::format_text(format, ##__VA_ARGS__); \
        std::string _line = ::chatnow::infra::build_log_line(                  \
            level_str, _msg,                                                   \
            {{"file", chatnow::basename_of(__FILE__)},                         \
             {"line", std::to_string(__LINE__)}});                             \
        chatnow::g_default_logger->log(level_enum, "{}", _line);               \
    } while (0)

#define LOG_TRACE(format, ...) _CN_LOG_JSON(spdlog::level::trace,    "trace", format, ##__VA_ARGS__)
#define LOG_DEBUG(format, ...) _CN_LOG_JSON(spdlog::level::debug,    "debug", format, ##__VA_ARGS__)
#define LOG_INFO(format, ...)  _CN_LOG_JSON(spdlog::level::info,     "info",  format, ##__VA_ARGS__)
#define LOG_WARN(format, ...)  _CN_LOG_JSON(spdlog::level::warn,     "warn",  format, ##__VA_ARGS__)
#define LOG_ERROR(format, ...) _CN_LOG_JSON(spdlog::level::err,      "error", format, ##__VA_ARGS__)
#define LOG_FATAL(format, ...) _CN_LOG_JSON(spdlog::level::critical, "fatal", format, ##__VA_ARGS__)

/* ============================================================
 *  字段化变体 —— event 为 msg；fields 由调用方传 initializer_list
 *  用法： LOG_INFOF("consumed_db_message",
 *           {{"message_id", std::to_string(id)},
 *            {"latency_ms", std::to_string(t)}});
 *  注意：value 必须是 std::string 已字符串化的形态
 * ============================================================ */
#define _CN_LOG_JSON_F(level_enum, level_str, event, fields_init)              \
    do {                                                                       \
        if (!chatnow::g_default_logger) break;                                 \
        std::string _line = ::chatnow::infra::build_log_line(                  \
            level_str, event, fields_init);                                    \
        chatnow::g_default_logger->log(level_enum, "{}", _line);               \
    } while (0)

#define LOG_TRACEF(event, fields) _CN_LOG_JSON_F(spdlog::level::trace,    "trace", event, fields)
#define LOG_DEBUGF(event, fields) _CN_LOG_JSON_F(spdlog::level::debug,    "debug", event, fields)
#define LOG_INFOF(event,  fields) _CN_LOG_JSON_F(spdlog::level::info,     "info",  event, fields)
#define LOG_WARNF(event,  fields) _CN_LOG_JSON_F(spdlog::level::warn,     "warn",  event, fields)
#define LOG_ERRORF(event, fields) _CN_LOG_JSON_F(spdlog::level::err,      "error", event, fields)
#define LOG_FATALF(event, fields) _CN_LOG_JSON_F(spdlog::level::critical, "fatal", event, fields)
