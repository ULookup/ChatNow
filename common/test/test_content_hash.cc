#include <gtest/gtest.h>
#include <string>
#include "utils/content_hash.hpp"

namespace ch = chatnow::content_hash;

TEST(ContentHash, ValidatesFormat) {
    EXPECT_TRUE(ch::is_valid("sha256:" + std::string(64, 'a')));
    EXPECT_TRUE(ch::is_valid("sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"));
    EXPECT_FALSE(ch::is_valid(""));
    EXPECT_FALSE(ch::is_valid("md5:abc"));
    EXPECT_FALSE(ch::is_valid("sha256:zzz"));
    EXPECT_FALSE(ch::is_valid("sha256:" + std::string(63, 'a')));
    // 大写 hex 不接受（强制 lower）
    EXPECT_FALSE(ch::is_valid("sha256:" + std::string(64, 'A')));
    // 长度多 1 字节
    EXPECT_FALSE(ch::is_valid("sha256:" + std::string(65, 'a')));
}

TEST(ContentHash, ExtractsHex) {
    auto hex = ch::hex_part("sha256:" + std::string(64, 'b'));
    ASSERT_EQ(hex.size(), 64u);
    EXPECT_EQ(hex[0], 'b');
    // 无效输入返回空
    EXPECT_TRUE(ch::hex_part("md5:abc").empty());
}

TEST(ContentHash, ShortPrefix) {
    auto h = "sha256:" + std::string(64, 'c');
    EXPECT_EQ(ch::short_prefix(h, 2), "cc");
    EXPECT_EQ(ch::short_prefix(h, 4), "cccc");
    // 无效 → 空
    EXPECT_TRUE(ch::short_prefix("not-a-hash", 2).empty());
}
