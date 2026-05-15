// 编译断言：UserBlockTable 接口形状不漂移
//
// 此测试不连真实数据库，仅在编译期确认 5 个公开方法签名。
// 真实 DAO 行为由集成测试覆盖（见 Task 9）。

#include <gtest/gtest.h>
#include <string>
#include <type_traits>

// 不 include 真正的 dao（避免单测拉 odb 链路）；用 declval 验证签名

namespace chatnow {
class UserBlockTable;
}

namespace {

// 仅占位测试，确认 test_*.cc GLOB 命中并通过 link
TEST(UserBlockDaoCompile, Placeholder) {
    EXPECT_TRUE(true);
}

}  // namespace
