#include <gtest/gtest.h>
#include <string>
#include "utils/mime_whitelist.hpp"

using chatnow::MimeWhitelist;

static const char* kJson = R"([
  {"prefix":"image/jpeg",      "max_mb":20},
  {"prefix":"image/png",       "max_mb":20},
  {"prefix":"video/mp4",       "max_mb":500},
  {"prefix":"audio/aac",       "max_mb":50},
  {"prefix":"application/pdf", "max_mb":100},
  {"prefix":"text/plain",      "max_mb":5}
])";

TEST(MimeWhitelist, ParsesJson) {
    MimeWhitelist wl;
    ASSERT_TRUE(wl.load_json(kJson));
    EXPECT_TRUE(wl.is_allowed("image/jpeg", 1024));
    EXPECT_TRUE(wl.is_allowed("image/jpeg", 20LL * 1024 * 1024));            // 边界
    EXPECT_FALSE(wl.is_allowed("image/jpeg", 20LL * 1024 * 1024 + 1));
    EXPECT_TRUE(wl.is_allowed("video/mp4",  400LL * 1024 * 1024));
    EXPECT_FALSE(wl.is_allowed("video/mp4", 600LL * 1024 * 1024));
    EXPECT_FALSE(wl.is_allowed("image/heic", 1));
    EXPECT_FALSE(wl.is_allowed("image/jpeg", 0));                            // size=0 拒绝
    EXPECT_EQ(wl.max_size("image/jpeg"), 20LL * 1024 * 1024);
    EXPECT_EQ(wl.max_size("image/heic"), -1);
}

TEST(MimeWhitelist, RejectsInvalidJson) {
    MimeWhitelist wl;
    EXPECT_FALSE(wl.load_json("not-json"));
}

TEST(MimeWhitelist, RejectsNonArrayRoot) {
    MimeWhitelist wl;
    EXPECT_FALSE(wl.load_json(R"({"prefix":"image/jpeg","max_mb":20})"));
}

TEST(MimeWhitelist, RejectsMalformedEntry) {
    MimeWhitelist wl;
    // 缺 max_mb
    EXPECT_FALSE(wl.load_json(R"([{"prefix":"image/jpeg"}])"));
    // max_mb 不是数字
    EXPECT_FALSE(wl.load_json(R"([{"prefix":"image/jpeg","max_mb":"20"}])"));
    // max_mb 为 0
    EXPECT_FALSE(wl.load_json(R"([{"prefix":"image/jpeg","max_mb":0}])"));
}

TEST(MimeWhitelist, FailedLoadDoesNotReplaceExisting) {
    MimeWhitelist wl;
    ASSERT_TRUE(wl.load_json(R"([{"prefix":"image/jpeg","max_mb":20}])"));
    EXPECT_TRUE(wl.is_allowed("image/jpeg", 1));
    // 失败 load 不应清掉旧白名单
    EXPECT_FALSE(wl.load_json("not-json"));
    EXPECT_TRUE(wl.is_allowed("image/jpeg", 1));
}
