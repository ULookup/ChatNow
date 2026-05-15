#include <gtest/gtest.h>
#include <string>
#include "utils/avatar_url.hpp"

namespace au = chatnow::avatar_url;

TEST(AvatarUrl, BasicHttp) {
    EXPECT_EQ(au::of("http://127.0.0.1:9000/chatnow-media-public", "abcd1234"),
              "http://127.0.0.1:9000/chatnow-media-public/avatar/abcd1234");
}

TEST(AvatarUrl, TrailingSlashStripped) {
    EXPECT_EQ(au::of("https://cdn.example.com/", "deadbeef"),
              "https://cdn.example.com/avatar/deadbeef");
}

TEST(AvatarUrl, EmptyPrefix) {
    // 边界：调用方传入空前缀（不推荐但不能崩）
    EXPECT_EQ(au::of("", "x"), "/avatar/x");
}

TEST(AvatarUrl, EmptyFileId) {
    // 边界：调用方传入空 file_id（不推荐但不能崩）
    EXPECT_EQ(au::of("https://cdn", ""), "https://cdn/avatar/");
}
