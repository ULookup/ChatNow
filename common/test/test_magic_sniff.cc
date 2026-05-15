#include <gtest/gtest.h>
#include <initializer_list>
#include <string>
#include "utils/magic_sniff.hpp"

namespace ms = chatnow::magic_sniff;

static std::string bytes(std::initializer_list<unsigned char> b) {
    return std::string(b.begin(), b.end());
}

TEST(MagicSniff, JPEG)  {
    EXPECT_EQ(ms::sniff(bytes({0xFF, 0xD8, 0xFF, 0xE0, 0, 0, 0, 0})), "image/jpeg");
}
TEST(MagicSniff, PNG)   {
    EXPECT_EQ(ms::sniff(bytes({0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A})), "image/png");
}
TEST(MagicSniff, GIF87) {
    EXPECT_EQ(ms::sniff(bytes({'G', 'I', 'F', '8', '7', 'a', 0, 0})), "image/gif");
}
TEST(MagicSniff, GIF89) {
    EXPECT_EQ(ms::sniff(bytes({'G', 'I', 'F', '8', '9', 'a', 0, 0})), "image/gif");
}
TEST(MagicSniff, WebP)  {
    auto b = std::string("RIFF\0\0\0\0WEBP", 12);
    EXPECT_EQ(ms::sniff(b), "image/webp");
}
TEST(MagicSniff, PE)    {
    EXPECT_EQ(ms::sniff(bytes({'M', 'Z', 0, 0, 0, 0, 0, 0})), "application/x-dosexec");
}
TEST(MagicSniff, ELF)   {
    EXPECT_EQ(ms::sniff(bytes({0x7F, 'E', 'L', 'F', 0, 0, 0, 0})), "application/x-elf");
}
TEST(MagicSniff, MachO_32_BE) {
    EXPECT_EQ(ms::sniff(bytes({0xFE, 0xED, 0xFA, 0xCE, 0, 0, 0, 0})), "application/x-mach-binary");
}
TEST(MagicSniff, MachO_32_LE) {
    EXPECT_EQ(ms::sniff(bytes({0xCE, 0xFA, 0xED, 0xFE, 0, 0, 0, 0})), "application/x-mach-binary");
}
TEST(MagicSniff, MachO_64_BE) {
    EXPECT_EQ(ms::sniff(bytes({0xFE, 0xED, 0xFA, 0xCF, 0, 0, 0, 0})), "application/x-mach-binary");
}
TEST(MagicSniff, MachO_64_LE) {
    EXPECT_EQ(ms::sniff(bytes({0xCF, 0xFA, 0xED, 0xFE, 0, 0, 0, 0})), "application/x-mach-binary");
}
TEST(MagicSniff, UnknownReturnsEmpty) {
    EXPECT_EQ(ms::sniff(std::string(8, 'a')), "");
}
TEST(MagicSniff, ShortBufferReturnsEmpty) {
    EXPECT_EQ(ms::sniff(std::string("ab")), "");
}

TEST(MagicSniff, MatchesClaimed_OK) {
    auto b = bytes({0xFF, 0xD8, 0xFF, 0xE0, 0, 0, 0, 0});
    EXPECT_TRUE(ms::matches_claimed(b, "image/jpeg"));
}
TEST(MagicSniff, MatchesClaimed_DangerousMimeMismatch) {
    // 客户端声称 image/jpeg，但实际是 PE
    auto b = bytes({'M', 'Z', 0, 0, 0, 0, 0, 0});
    EXPECT_FALSE(ms::matches_claimed(b, "image/jpeg"));
}
TEST(MagicSniff, MatchesClaimed_DangerousAlwaysFalse) {
    // 即使客户端声称 application/x-dosexec，也拒绝
    auto b = bytes({'M', 'Z', 0, 0, 0, 0, 0, 0});
    EXPECT_FALSE(ms::matches_claimed(b, "application/x-dosexec"));
}
TEST(MagicSniff, MatchesClaimed_UnknownIsLenient) {
    // 检测不到（未知格式）→ 保守放行
    EXPECT_TRUE(ms::matches_claimed(std::string(8, 'a'), "application/octet-stream"));
}
TEST(MagicSniff, MatchesClaimed_ImageMismatch) {
    auto b = bytes({0xFF, 0xD8, 0xFF, 0xE0, 0, 0, 0, 0});
    EXPECT_FALSE(ms::matches_claimed(b, "image/png"));
}
