#include <gtest/gtest.h>
#include <string>
#include "utils/object_key.hpp"

namespace ok = chatnow::object_key;

TEST(ObjectKey, ChatPrefixIncludesDate) {
    // 1715644800000 ms = 2024-05-14 00:00:00 UTC
    auto h = std::string("sha256:abcd") + std::string(60, 'e');
    auto k = ok::build("chat", h, 1715644800000LL);
    EXPECT_EQ(k, "chat/2024/05/14/ab/abcd" + std::string(60, 'e'));
}

TEST(ObjectKey, AvatarPrefixHasNoDate) {
    auto h = std::string("sha256:") + std::string(64, '1');
    auto k = ok::build_flat("avatar", h);
    EXPECT_EQ(k, "avatar/" + std::string(64, '1'));
}

TEST(ObjectKey, YyyymmddIsUtc) {
    // 1700000000000 ms = 2023-11-14 22:13:20 UTC
    EXPECT_EQ(ok::yyyymmdd_slashed(1700000000000LL), "2023/11/14");
    // 1672531200000 ms = 2023-01-01 00:00:00 UTC
    EXPECT_EQ(ok::yyyymmdd_slashed(1672531200000LL), "2023/01/01");
}

TEST(ObjectKey, EmptyHashYieldsEmptyHexShortPrefix) {
    auto k = ok::build("chat", "not-a-hash", 1715644800000LL);
    // hex_part 会返回空串；substr(0,2) 也空；输出 "chat/2024/05/14//"
    EXPECT_EQ(k, "chat/2024/05/14//");
}
