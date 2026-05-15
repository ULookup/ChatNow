// 占位编译断言：仅用于占住 common/test/CMakeLists.txt 的 test_*.cc GLOB。
//
// UserBlockTable 真实签名验证留给 Task 10 集成测试（连真实 MySQL）。
// 此 TU 故意不包含任何 DAO header，避免把 odb 链路拉进单测。

#include <gtest/gtest.h>

namespace chatnow {
class UserBlockTable;
}

namespace {

TEST(UserBlockDaoCompile, Placeholder) {
    EXPECT_TRUE(true);
}

}  // namespace
