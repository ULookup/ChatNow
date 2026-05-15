#include "log/log_context.hpp"
#include <gtest/gtest.h>
#include <thread>

using chatnow::log::LogContext;

class LogContextTest : public ::testing::Test {
protected:
    void TearDown() override { LogContext::clear(); }
};

TEST_F(LogContextTest, EmptyByDefault) {
    EXPECT_TRUE(LogContext::current().empty());
    EXPECT_EQ(LogContext::format_prefix(), "");
}

TEST_F(LogContextTest, SetAndRead) {
    LogContext::set("trace-1", "user-2", "device-3");
    EXPECT_EQ(LogContext::current().trace_id,  "trace-1");
    EXPECT_EQ(LogContext::current().user_id,   "user-2");
    EXPECT_EQ(LogContext::current().device_id, "device-3");
}

TEST_F(LogContextTest, FormatPrefixAllFields) {
    LogContext::set("t", "u", "d");
    EXPECT_EQ(LogContext::format_prefix(), "[trace=t user=u device=d] ");
}

TEST_F(LogContextTest, FormatPrefixPartial) {
    LogContext::set("t", "", "d");
    EXPECT_EQ(LogContext::format_prefix(), "[trace=t device=d] ");
}

TEST_F(LogContextTest, ClearResets) {
    LogContext::set("t","u","d");
    LogContext::clear();
    EXPECT_TRUE(LogContext::current().empty());
}

TEST_F(LogContextTest, ThreadLocalIsolation) {
    LogContext::set("main-trace","main-user","main-dev");

    std::string other_trace_at_set;
    std::thread t([&]() {
        // 子线程刚启动时应当为空（bthread_key 在纯 pthread 模式下退化为 TLS，各自独立）
        other_trace_at_set = LogContext::current().trace_id;
        LogContext::set("child-trace","child-user","child-dev");
    });
    t.join();

    EXPECT_EQ(other_trace_at_set, "");
    EXPECT_EQ(LogContext::current().trace_id, "main-trace");
}
