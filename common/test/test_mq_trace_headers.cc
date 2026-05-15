// common/test/test_mq_trace_headers.cc
#include "mq/trace_headers.hpp"
#include "log/log_context.hpp"
#include <gtest/gtest.h>

using namespace chatnow::mq;
using chatnow::log::LogContext;

class MqTraceHeadersTest : public ::testing::Test {
protected:
    void TearDown() override { LogContext::clear(); }
};

TEST_F(MqTraceHeadersTest, InjectFromLogContext) {
    LogContext::set("trace-abc", "u", "d");
    std::map<std::string, std::string> h;
    mq_inject_trace_headers(h);
    EXPECT_EQ(h[kTraceHeader], "trace-abc");
}

TEST_F(MqTraceHeadersTest, InjectSkipsWhenContextEmpty) {
    std::map<std::string, std::string> h;
    mq_inject_trace_headers(h);
    EXPECT_EQ(h.find(kTraceHeader), h.end());
}

TEST_F(MqTraceHeadersTest, InjectPreservesOtherHeaders) {
    LogContext::set("trace-xyz", "", "");
    std::map<std::string, std::string> h{{"existing", "val"}};
    mq_inject_trace_headers(h);
    EXPECT_EQ(h["existing"], "val");
    EXPECT_EQ(h[kTraceHeader], "trace-xyz");
}

TEST_F(MqTraceHeadersTest, ExtractPresent) {
    std::map<std::string, std::string> h{{kTraceHeader, "tt"}};
    EXPECT_EQ(mq_extract_trace_id(h), "tt");
}

TEST_F(MqTraceHeadersTest, ExtractMissingReturnsEmpty) {
    std::map<std::string, std::string> h;
    EXPECT_EQ(mq_extract_trace_id(h), "");
}

TEST_F(MqTraceHeadersTest, ExtractIgnoresOtherKeys) {
    std::map<std::string, std::string> h{{"foo", "bar"}};
    EXPECT_EQ(mq_extract_trace_id(h), "");
}
