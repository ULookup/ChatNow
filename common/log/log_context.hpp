#pragma once

/**
 * LogContext —— RPC 调用链结构化字段（MDC）
 * ---
 * 每个 RPC handler 入口由 HANDLE_RPC 宏调用 LogContext::set(...)，
 * 退出时 clear()。线程内的所有日志可通过 LogContext::current() 获取
 * trace_id / user_id / device_id 自动拼接。
 *
 * 设计：
 *   - 使用 brpc 的 bthread_key_* 原语存储字段。brpc 在 M:N 模式下
 *     bthread 可能在挂起点（RPC/锁/sleep）跨 pthread 迁移，普通的
 *     thread_local 绑定底层 pthread，迁移后会读到错误线程的状态，
 *     导致 in-flight RPC 之间日志字段串味。bthread_key 是 bthread
 *     感知的：每个 bthread 独立 slot，迁移安全；从纯 pthread 调用
 *     时，bthread 库会透明回退到 thread_local 语义。
 *   - 不依赖 spdlog，独立成单元；P8 trace_id 全链路 plan 会把这些字段
 *     接入 spdlog formatter。本 plan 仅提供存储与读取。
 *   - format_prefix() 输出 "[trace=xxx user=yyy device=zzz] "，业务侧
 *     若想在 LOG_INFO 里手动前缀化可用（P8 之后将自动前缀化）。
 *   - 字段缺失时不写入，不输出空 [trace=]。
 *   - clear() 只重置字段内容、保留分配（同一 bthread 复用），bthread
 *     结束时由 bthread_key_create 注册的 dtor 释放 LogFields。
 */

#include <bthread/bthread.h>

#include <cassert>
#include <string>

namespace chatnow::log {

struct LogFields {
    std::string trace_id;
    std::string user_id;
    std::string device_id;

    bool empty() const { return trace_id.empty() && user_id.empty() && device_id.empty(); }
};

namespace detail {

inline void log_fields_dtor(void* p) {
    delete static_cast<LogFields*>(p);
}

inline bthread_key_t& log_fields_key() {
    struct KeyInit {
        bthread_key_t key;
        KeyInit() {
            assert(0 == bthread_key_create(&key, &log_fields_dtor));
        }
    };
    static KeyInit init;
    return init.key;
}

inline LogFields* get_log_fields_or_null() {
    return static_cast<LogFields*>(bthread_getspecific(log_fields_key()));
}

inline LogFields* get_or_create_log_fields() {
    LogFields* p = get_log_fields_or_null();
    if (!p) {
        p = new LogFields();
        bthread_setspecific(log_fields_key(), p);
    }
    return p;
}

inline const LogFields& empty_log_fields() {
    static const LogFields kEmpty;
    return kEmpty;
}

}  // namespace detail

class LogContext {
public:
    static void set(const std::string& trace_id,
                    const std::string& user_id,
                    const std::string& device_id) {
        LogFields* f = detail::get_or_create_log_fields();
        f->trace_id  = trace_id;
        f->user_id   = user_id;
        f->device_id = device_id;
    }

    /* 重置字段内容；不释放 LogFields，以便同一 bthread 上后续请求复用。
       LogFields 的实际销毁由 bthread_key_create 注册的析构在 bthread 结束时执行。 */
    static void clear() {
        if (LogFields* f = detail::get_log_fields_or_null()) {
            f->trace_id.clear();
            f->user_id.clear();
            f->device_id.clear();
        }
    }

    /* 读路径必须 O(1)、无副作用：未设置时返回静态空对象，不分配。 */
    static const LogFields& current() {
        if (const LogFields* f = detail::get_log_fields_or_null()) {
            return *f;
        }
        return detail::empty_log_fields();
    }

    /* brief: 拼接 "[trace=xxx user=yyy device=zzz] "；缺失字段省略；全空返回 "" */
    static std::string format_prefix() {
        const LogFields& f = current();
        if (f.empty()) return "";
        std::string out = "[";
        bool need_space = false;
        if (!f.trace_id.empty())  { out += "trace="  + f.trace_id;  need_space = true; }
        if (!f.user_id.empty())   { if (need_space) out += " "; out += "user="   + f.user_id;   need_space = true; }
        if (!f.device_id.empty()) { if (need_space) out += " "; out += "device=" + f.device_id; }
        out += "] ";
        return out;
    }
};

}  // namespace chatnow::log
