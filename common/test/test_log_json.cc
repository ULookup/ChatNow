// common/test/test_log_json.cc
#include "infra/log_json.hpp"
#include "log/log_context.hpp"
#include <gtest/gtest.h>

using chatnow::infra::escape_json;
using chatnow::infra::build_log_line;
using chatnow::infra::set_service_name;
using chatnow::log::LogContext;

class LogJsonTest : public ::testing::Test {
protected:
    void SetUp() override { LogContext::clear(); set_service_name("testsvc"); }
    void TearDown() override { LogContext::clear(); }
};

TEST_F(LogJsonTest, EscapeQuoteAndBackslash) {
    EXPECT_EQ(escape_json("a\"b"),  "a\\\"b");
    EXPECT_EQ(escape_json("a\\b"),  "a\\\\b");
}

TEST_F(LogJsonTest, EscapeControlChars) {
    EXPECT_EQ(escape_json("a\nb"), "a\\nb");
    EXPECT_EQ(escape_json("a\tb"), "a\\tb");
    EXPECT_EQ(escape_json("a\rb"), "a\\rb");
    EXPECT_EQ(escape_json("a\bb"), "a\\bb");
    EXPECT_EQ(escape_json("a\fb"), "a\\fb");
}

TEST_F(LogJsonTest, EscapeOtherControlAsUnicode) {
    // 0x01 → 
    std::string in;
    in.push_back(static_cast<char>(0x01));
    EXPECT_EQ(escape_json(in), "\\u0001");
}

TEST_F(LogJsonTest, NonAsciiPassthrough) {
    // UTF-8 中文不应转义
    std::string s = "你好";
    EXPECT_EQ(escape_json(s), s);
}

TEST_F(LogJsonTest, BuildLineHasRequiredKeys) {
    LogContext::set("trace1", "u1", "d1");
    auto line = build_log_line("info", "msg1");
    EXPECT_NE(line.find("\"level\":\"info\""),       std::string::npos);
    EXPECT_NE(line.find("\"service\":\"testsvc\""),  std::string::npos);
    EXPECT_NE(line.find("\"trace_id\":\"trace1\""),  std::string::npos);
    EXPECT_NE(line.find("\"user_id\":\"u1\""),       std::string::npos);
    EXPECT_NE(line.find("\"device_id\":\"d1\""),     std::string::npos);
    EXPECT_NE(line.find("\"msg\":\"msg1\""),         std::string::npos);
    EXPECT_NE(line.find("\"fields\":{}"),            std::string::npos);
    EXPECT_NE(line.find("\"ts\":\""),                std::string::npos);
}

TEST_F(LogJsonTest, BuildLineFields) {
    auto line = build_log_line("warn", "evt", {{"k1","v1"}, {"k2","v2"}});
    EXPECT_NE(line.find("\"fields\":{\"k1\":\"v1\",\"k2\":\"v2\"}"),
              std::string::npos);
}

TEST_F(LogJsonTest, BuildLineEscapesMsgAndFieldValue) {
    auto line = build_log_line("info", "msg \"q\" \\back",
                               {{"k","v\nl2"}});
    EXPECT_NE(line.find("\"msg\":\"msg \\\"q\\\" \\\\back\""),
              std::string::npos);
    EXPECT_NE(line.find("\"k\":\"v\\nl2\""),
              std::string::npos);
}

TEST_F(LogJsonTest, BuildLineBalancedBraces) {
    // 简易"JSON 语法体检"：{ 与 } 数量平衡
    auto line = build_log_line("info", "x", {{"a","b"}});
    int open = 0, close = 0;
    for (char c : line) { if (c == '{') ++open; if (c == '}') ++close; }
    EXPECT_EQ(open, close);
    EXPECT_EQ(open, 2);  // 顶层 {} + fields {}
}

TEST_F(LogJsonTest, EmptyContextStillProducesKeys) {
    auto line = build_log_line("debug", "x");
    EXPECT_NE(line.find("\"trace_id\":\"\""),  std::string::npos);
    EXPECT_NE(line.find("\"user_id\":\"\""),   std::string::npos);
    EXPECT_NE(line.find("\"device_id\":\"\""), std::string::npos);
}
