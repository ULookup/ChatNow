#pragma once

/**
 * LogContext —— RPC 调用链结构化字段（MDC）
 * ---
 * 每个 RPC handler 入口由 HANDLE_RPC 宏调用 LogContext::set(...)，
 * 退出时 clear()。线程内的所有日志可通过 LogContext::current() 获取
 * trace_id / user_id / device_id 自动拼接。
 *
 * 设计：
 *   - thread_local 存储；brpc 默认每个 RPC 独占一个线程（pthread 模式）
 *     或一个 bthread（M:N 模式）。bthread 也支持 thread_local 语义
 *     （bthread 切换时不会跨 thread_local），所以本设计在 brpc M:N
 *     模式下也是安全的。
 *   - 不依赖 spdlog，独立成单元；P8 trace_id 全链路 plan 会把这些字段
 *     接入 spdlog formatter。本 plan 仅提供存储与读取。
 *   - format_prefix() 输出 "[trace=xxx user=yyy device=zzz] "，业务侧
 *     若想在 LOG_INFO 里手动前缀化可用（P8 之后将自动前缀化）。
 *   - 字段缺失时不写入，不输出空 [trace=]。
 */

#include <string>

namespace chatnow::log {

struct LogFields {
    std::string trace_id;
    std::string user_id;
    std::string device_id;

    bool empty() const { return trace_id.empty() && user_id.empty() && device_id.empty(); }
};

class LogContext {
public:
    static void set(const std::string& trace_id,
                    const std::string& user_id,
                    const std::string& device_id) {
        auto& f = local();
        f.trace_id  = trace_id;
        f.user_id   = user_id;
        f.device_id = device_id;
    }

    static void clear() {
        auto& f = local();
        f.trace_id.clear();
        f.user_id.clear();
        f.device_id.clear();
    }

    static const LogFields& current() { return local(); }

    /* brief: 拼接 "[trace=xxx user=yyy device=zzz] "；缺失字段省略；全空返回 "" */
    static std::string format_prefix() {
        const auto& f = local();
        if (f.empty()) return "";
        std::string out = "[";
        bool need_space = false;
        if (!f.trace_id.empty())  { out += "trace="  + f.trace_id;  need_space = true; }
        if (!f.user_id.empty())   { if (need_space) out += " "; out += "user="   + f.user_id;   need_space = true; }
        if (!f.device_id.empty()) { if (need_space) out += " "; out += "device=" + f.device_id; }
        out += "] ";
        return out;
    }

private:
    static LogFields& local() {
        thread_local LogFields fields;
        return fields;
    }
};

}  // namespace chatnow::log
