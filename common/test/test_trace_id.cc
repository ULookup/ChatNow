// common/test/test_trace_id.cc
#include "utils/trace_id.hpp"
#include <gtest/gtest.h>
#include <set>

using chatnow::utils::gen_trace_id;
using chatnow::utils::is_valid_trace_id;

TEST(TraceId, FormatLength32) {
    auto t = gen_trace_id();
    EXPECT_EQ(t.size(), 32u);
}

TEST(TraceId, AllHexLowercase) {
    auto t = gen_trace_id();
    for (char c : t) {
        bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        EXPECT_TRUE(ok) << "non-hex char: " << c;
    }
}

TEST(TraceId, IsValidPositive) {
    EXPECT_TRUE(is_valid_trace_id("0123456789abcdef0123456789abcdef"));
    EXPECT_TRUE(is_valid_trace_id(gen_trace_id()));
}

TEST(TraceId, IsValidNegativeWrongLen) {
    EXPECT_FALSE(is_valid_trace_id(""));
    EXPECT_FALSE(is_valid_trace_id("abc"));
    EXPECT_FALSE(is_valid_trace_id(std::string(31, 'a')));
    EXPECT_FALSE(is_valid_trace_id(std::string(33, 'a')));
}

TEST(TraceId, IsValidNegativeUppercase) {
    EXPECT_FALSE(is_valid_trace_id("0123456789ABCDEF0123456789abcdef"));
}

TEST(TraceId, IsValidNegativeNonHex) {
    EXPECT_FALSE(is_valid_trace_id("0123456789abcdef0123456789abcdez"));
    EXPECT_FALSE(is_valid_trace_id("0123456789abcdef0123456789abcde "));
}

TEST(TraceId, UniquenessOver1000Calls) {
    std::set<std::string> seen;
    for (int i = 0; i < 1000; ++i) {
        seen.insert(gen_trace_id());
    }
    // 概率上 1000 个 128bit 随机值碰撞接近 0
    EXPECT_EQ(seen.size(), 1000u);
}
