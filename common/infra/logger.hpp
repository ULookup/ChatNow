#pragma once

/**
 * ===========================================================================
 * 日志封装（基于 spdlog）
 * ---------------------------------------------------------------------------
 * 设计要点：
 *   1. 全局 logger 改为 inline 变量，避免多 TU include 时重复定义链接错误
 *   2. 发布模式启用「异步日志 + rotating_file_sink」：
 *      - 单文件 50MB，轮转保留 10 份；避免单文件无限膨胀
 *      - 异步队列 8192，一个后台线程；日志路径不阻塞业务
 *   3. flush 策略：调试模式 trace 级即刷；发布模式仅 warn 级别以上即刷，
 *      其他级别每 3 秒批量刷盘一次，兼顾性能与故障可见性
 *   4. 日志格式补充毫秒精度时间戳 + 线程 ID + 文件名(去路径)
 *   5. LOG_xxx 宏使用 __builtin_strrchr 抽出 basename，避免日志冗长
 *
 * 使用：
 *   chatnow::init_logger(true, "/im/logs/user.log", spdlog::level::info);
 *   LOG_INFO("user logged in: {}", uid);
 * ===========================================================================
 */

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/async.h>
#include <chrono>
#include <memory>
#include <string>

namespace chatnow
{

// inline 变量保证 ODR：多个 .cc 包含本头文件不会产生重复定义
inline std::shared_ptr<spdlog::logger> g_default_logger;

// 发布模式日志文件大小阈值与保留份数
inline constexpr size_t kMaxFileSize  = 50 * 1024 * 1024;  // 50MB
inline constexpr size_t kMaxFileCount = 10;
inline constexpr size_t kAsyncQueue   = 8192;

/* brief: 初始化全局日志器
 *  @param mode      true=发布模式（文件 + 异步） / false=调试模式（控制台 + 同步）
 *  @param filename  发布模式输出文件
 *  @param level     发布模式日志等级（spdlog level enum 整数）
 */
inline void init_logger(bool mode, const std::string &filename, int32_t level) {
    if(mode == false) {
        // 调试模式：彩色控制台输出，最低等级，立即刷盘便于现场调试
        spdlog::drop("console-logger");
        g_default_logger = spdlog::stdout_color_mt("console-logger");
        g_default_logger->set_level(spdlog::level::trace);
        g_default_logger->flush_on(spdlog::level::trace);
    } else {
        // 发布模式：异步 rotating file，避免单文件膨胀 / 同步 IO 阻塞业务线程
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
        // warn/error/critical 立即刷；其它级别 3s 批量刷
        g_default_logger->flush_on(spdlog::level::warn);
        spdlog::flush_every(std::chrono::seconds(3));
    }
    // 模式: [logger名][HH:MM:SS.ms][线程ID][级别] 内容
    g_default_logger->set_pattern("[%n][%H:%M:%S.%e][%t]%^[%l]%$ %v");
}

/* brief: 抽取文件 basename，避免日志中出现绝对路径噪音 */
constexpr const char* basename_of(const char* path) {
    const char* last = path;
    for(const char* p = path; *p; ++p) {
        if(*p == '/' || *p == '\\') last = p + 1;
    }
    return last;
}

#define LOG_TRACE(format, ...) chatnow::g_default_logger->trace   (std::string("[{}:{}] ") + format, chatnow::basename_of(__FILE__), __LINE__, ##__VA_ARGS__)
#define LOG_DEBUG(format, ...) chatnow::g_default_logger->debug   (std::string("[{}:{}] ") + format, chatnow::basename_of(__FILE__), __LINE__, ##__VA_ARGS__)
#define LOG_INFO(format, ...)  chatnow::g_default_logger->info    (std::string("[{}:{}] ") + format, chatnow::basename_of(__FILE__), __LINE__, ##__VA_ARGS__)
#define LOG_WARN(format, ...)  chatnow::g_default_logger->warn    (std::string("[{}:{}] ") + format, chatnow::basename_of(__FILE__), __LINE__, ##__VA_ARGS__)
#define LOG_ERROR(format, ...) chatnow::g_default_logger->error   (std::string("[{}:{}] ") + format, chatnow::basename_of(__FILE__), __LINE__, ##__VA_ARGS__)
#define LOG_FATAL(format, ...) chatnow::g_default_logger->critical(std::string("[{}:{}] ") + format, chatnow::basename_of(__FILE__), __LINE__, ##__VA_ARGS__)

} // namespace chatnow
