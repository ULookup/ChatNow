# Phase 0a: 测试基础设施 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 搭建 ChatNow 测试基础设施（docker-compose、CI workflow、CTest 集成、共享 fixtures、E2E helpers、mocks 目录），使 CI 能跑通 dummy 测试，为 Phase 0b（接口抽取）和 Phase 1（业务测试）提供骨架。

**Architecture:** 新增 `docker/docker-compose.test.yml`（仅基础设施）、`.github/workflows/ci.yml`（三 job 串联）、`tests/` 目录（fixtures + mocks + e2e/helpers）。根 `CMakeLists.txt` 接入 CTest 并注册各 test 子目录。本 plan 不修改任何生产代码，仅新增测试基础设施文件。

**Tech Stack:** CMake 3.1+、CTest、GitHub Actions、Docker Compose、gtest + gmock、cpp-httplib、websocketpp

## Global Constraints

- 目标环境是 Linux（Ubuntu 22.04），开发在 macOS。所有构建命令和库假设以 Linux 为目标。
- 测试框架：gtest + gmock（不引入其他 mock 框架）。
- 接口与实现分文件：`i_*.hpp` 接口头 + `<concrete>.hpp` 实现头（来自 spec 2.2 节）。
- CI 单 workflow 三 job 串联：unit -> integration -> e2e（来自 spec 6.1 节）。
- 环境变量门控：`DB_TEST` / `REDIS_TEST` / `ES_TEST` / `MQ_TEST` / `MINIO_TEST` / `E2E_TEST`（来自 spec 4.2 节）。
- CTest labels：`unit` / `integration` / `e2e`（来自 spec 1.4 节）。
- 本 plan 不修改生产代码（`common/`、`transmite/`、`message/` 等服务源码）。接口抽取在 Phase 0b。

---

## File Structure

本 plan 新增以下文件，不修改任何现有源码：

```
docker/docker-compose.test.yml              # Task 1: 仅基础设施的 compose
scripts/wait_for_infra.sh                    # Task 1: 基础设施健康检查
scripts/wait_for_services.sh                 # Task 1: 业务服务健康检查
.github/workflows/ci.yml                     # Task 2: CI workflow
tests/CMakeLists.txt                         # Task 3: tests 顶层 CMake
tests/dummy/test_dummy_unit.cc               # Task 3: dummy unit 测试
tests/dummy/test_dummy_integration.cc        # Task 3: dummy integration 测试
tests/dummy/test_dummy_e2e.cc                # Task 3: dummy e2e 测试
tests/dummy/CMakeLists.txt                   # Task 3: dummy CMake
tests/fixtures/fake_closure.hpp              # Task 4: protobuf Closure 假实现
tests/fixtures/proto_helpers.hpp             # Task 4: protobuf 对象构造 helper
tests/fixtures/db_fixture.hpp                # Task 4: MySQL 测试 fixture
tests/fixtures/mq_fixture.hpp                # Task 4: RabbitMQ 测试 fixture
tests/fixtures/CMakeLists.txt                # Task 4: fixtures CMake（header-only）
tests/e2e/helpers/http_client.hpp            # Task 5: HTTP 客户端封装
tests/e2e/helpers/ws_client.hpp              # Task 5: WebSocket 客户端封装
tests/e2e/helpers/test_users.hpp             # Task 5: 测试用户 fixture
tests/e2e/CMakeLists.txt                     # Task 5: e2e CMake 骨架
tests/mocks/CMakeLists.txt                   # Task 6: mocks CMake（header-only）
CMakeLists.txt                               # Task 3: 根 CMake 接入 CTest（修改）
```

---

### Task 1: docker-compose.test.yml + 健康检查脚本

**Files:**
- Create: `docker/docker-compose.test.yml`
- Create: `scripts/wait_for_infra.sh`
- Create: `scripts/wait_for_services.sh`

**Interfaces:**
- Produces: `docker/docker-compose.test.yml`（Phase 0b/Phase 1 集成测试依赖此文件起基础设施）
- Produces: `scripts/wait_for_infra.sh`（CI integration job 和本地开发用）
- Produces: `scripts/wait_for_services.sh`（CI e2e job 和本地开发用）

- [ ] **Step 1: 创建 docker-compose.test.yml**

Create `docker/docker-compose.test.yml`:

```yaml
# 仅基础设施（不含业务服务），给集成测试用
# 用法：docker compose -f docker/docker-compose.test.yml up -d
services:
  mysql:
    image: mysql:8.0
    environment:
      MYSQL_ROOT_PASSWORD: chatnow_test
      MYSQL_DATABASE: chatnow_test
    ports:
      - "3306:3306"
    volumes:
      - ../sql:/docker-entrypoint-initdb.d
    healthcheck:
      test: ["CMD", "mysqladmin", "ping", "-h", "localhost"]
      interval: 3s
      timeout: 5s
      retries: 30

  redis:
    image: redis:7-alpine
    ports:
      - "6379:6379"
    healthcheck:
      test: ["CMD", "redis-cli", "ping"]
      interval: 3s
      timeout: 3s
      retries: 10

  elasticsearch:
    image: docker.elastic.co/elasticsearch/elasticsearch:7.17.0
    environment:
      - discovery.type=single-node
      - "ES_JAVA_OPTS=-Xms256m -Xmx256m"
    ports:
      - "9200:9200"
    healthcheck:
      test: ["CMD-SHELL", "curl -sf http://localhost:9200/_cluster/health || exit 1"]
      interval: 5s
      timeout: 5s
      retries: 20

  rabbitmq:
    image: rabbitmq:3-management
    ports:
      - "5672:5672"
      - "15672:15672"
    healthcheck:
      test: ["CMD", "rabbitmq-diagnostics", "check_running"]
      interval: 5s
      timeout: 5s
      retries: 20

  minio:
    image: minio/minio
    command: server /data
    ports:
      - "9000:9000"
    environment:
      MINIO_ROOT_USER: minioadmin
      MINIO_ROOT_PASSWORD: minioadmin
    healthcheck:
      test: ["CMD", "curl", "-sf", "http://localhost:9000/minio/health/live"]
      interval: 3s
      timeout: 5s
      retries: 20
```

- [ ] **Step 2: 创建 wait_for_infra.sh**

Create `scripts/wait_for_infra.sh`:

```bash
#!/bin/bash
# 轮询基础设施端口，全部就绪后退出 0，超时退出 1
# 用法：./scripts/wait_for_infra.sh [timeout_seconds]
set -euo pipefail

TIMEOUT=${1:-60}
START=$(date +%s)

check_port() {
    local host=$1 port=$2
    nc -z "$host" "$port" 2>/dev/null
}

wait_for() {
    local name=$1 host=$2 port=$3
    while ! check_port "$host" "$port"; do
        local now=$(date +%s)
        if [ $((now - START)) -gt $TIMEOUT ]; then
            echo "TIMEOUT: $name ($host:$port) 未就绪" >&2
            return 1
        fi
        echo "等待 $name ($host:$port)..."
        sleep 2
    done
    echo "$name ($host:$port) 就绪"
}

wait_for "MySQL" 127.0.0.1 3306
wait_for "Redis" 127.0.0.1 6379
wait_for "Elasticsearch" 127.0.0.1 9200
wait_for "RabbitMQ" 127.0.0.1 5672
wait_for "MinIO" 127.0.0.1 9000

echo "所有基础设施就绪"
```

- [ ] **Step 3: 创建 wait_for_services.sh**

Create `scripts/wait_for_services.sh`:

```bash
#!/bin/bash
# 轮询业务服务端口（gateway + 7 个微服务），全部就绪后退出 0，超时退出 1
# 用法：./scripts/wait_for_services.sh [timeout_seconds]
set -euo pipefail

TIMEOUT=${1:-120}
START=$(date +%s)

check_port() {
    local host=$1 port=$2
    nc -z "$host" "$port" 2>/dev/null
}

wait_for() {
    local name=$1 host=$2 port=$3
    while ! check_port "$host" "$port"; do
        local now=$(date +%s)
        if [ $((now - START)) -gt $TIMEOUT ]; then
            echo "TIMEOUT: $name ($host:$port) 未就绪" >&2
            return 1
        fi
        echo "等待 $name ($host:$port)..."
        sleep 2
    done
    echo "$name ($host:$port) 就绪"
}

wait_for "Gateway-HTTP" 127.0.0.1 9000
wait_for "Gateway-WS" 127.0.0.1 9001
wait_for "SpeechService" 127.0.0.1 10001
wait_for "FileService" 127.0.0.1 10002
wait_for "TransmiteService" 127.0.0.1 10004
wait_for "MessageService" 127.0.0.1 10005
wait_for "FriendService" 127.0.0.1 10006
wait_for "UserService" 127.0.0.1 10003

echo "所有业务服务就绪"
```

- [ ] **Step 4: 赋予脚本执行权限**

Run: `chmod +x scripts/wait_for_infra.sh scripts/wait_for_services.sh`
Expected: 无输出，`ls -la scripts/` 显示两个脚本有 `x` 权限。

- [ ] **Step 5: 验证 docker-compose.test.yml 语法**

Run: `docker compose -f docker/docker-compose.test.yml config >/dev/null`
Expected: 无错误输出，退出码 0。

- [ ] **Step 6: 提交**

```bash
git add docker/docker-compose.test.yml scripts/wait_for_infra.sh scripts/wait_for_services.sh
git commit -m "infra(test): docker-compose.test.yml + 健康检查脚本

- docker-compose.test.yml: 仅基础设施（mysql/redis/es/mq/minio），含 healthcheck
- wait_for_infra.sh: 轮询 5 个基础设施端口
- wait_for_services.sh: 轮询 gateway + 7 个业务服务端口"
```

---

### Task 2: CI workflow（ci.yml）

**Files:**
- Create: `.github/workflows/ci.yml`

**Interfaces:**
- Produces: `.github/workflows/ci.yml`（三 job 串联，Phase 0b/Phase 1 测试由此 CI 驱动）

- [ ] **Step 1: 创建 ci.yml**

Create `.github/workflows/ci.yml`:

```yaml
name: CI

on:
  push:
    branches: [main, develop]
  pull_request:
    branches: [main]
  schedule:
    - cron: "0 2 * * *"

jobs:
  unit:
    runs-on: ubuntu-22.04
    steps:
      - uses: actions/checkout@v4
      - name: Install dependencies
        run: |
          sudo apt-get update
          sudo apt-get install -y \
            libgtest-dev libgmock-dev libbrpc-dev libprotobuf-dev protobuf-compiler \
            libodb-dev libodb-mysql-dev libssl-dev libcurl4-openssl-dev \
            libjsoncpp-dev libboost-all-dev
      - name: Build unit tests
        run: |
          mkdir -p build && cd build
          cmake ..
          make -j$(nproc) common_tests dummy_unit_tests
      - name: Run unit tests
        run: cd build && ctest -L unit --output-on-failure

  integration:
    needs: unit
    runs-on: ubuntu-22.04
    steps:
      - uses: actions/checkout@v4
      - name: Install dependencies
        run: |
          sudo apt-get update
          sudo apt-get install -y \
            libgtest-dev libgmock-dev libbrpc-dev libprotobuf-dev protobuf-compiler \
            libodb-dev libodb-mysql-dev libssl-dev libcurl4-openssl-dev \
            libjsoncpp-dev libboost-all-dev
      - name: Start infrastructure
        run: docker compose -f docker/docker-compose.test.yml up -d
      - name: Wait for infrastructure
        run: ./scripts/wait_for_infra.sh
      - name: Build integration tests
        run: |
          mkdir -p build && cd build
          cmake ..
          make -j$(nproc) dummy_integration_tests
      - name: Run integration tests
        env:
          DB_TEST: "1"
          REDIS_TEST: "1"
          ES_TEST: "1"
          MQ_TEST: "1"
          MINIO_TEST: "1"
        run: cd build && ctest -L integration --output-on-failure
      - name: Tear down infrastructure
        if: always()
        run: docker compose -f docker/docker-compose.test.yml down -v

  e2e:
    needs: integration
    runs-on: ubuntu-22.04
    if: github.event_name == 'schedule' || (github.event_name == 'pull_request' && github.base_ref == 'main')
    steps:
      - uses: actions/checkout@v4
      - name: Install dependencies
        run: |
          sudo apt-get update
          sudo apt-get install -y \
            libgtest-dev libgmock-dev libbrpc-dev libprotobuf-dev protobuf-compiler \
            libodb-dev libodb-mysql-dev libssl-dev libcurl4-openssl-dev \
            libjsoncpp-dev libboost-all-dev
      - name: Start full stack
        run: docker compose up -d --build
      - name: Wait for services
        run: ./scripts/wait_for_services.sh
      - name: Build e2e tests
        run: |
          mkdir -p build && cd build
          cmake ..
          make -j$(nproc) dummy_e2e_tests
      - name: Run e2e tests
        env:
          E2E_TEST: "1"
        run: cd build && ctest -L e2e --output-on-failure
      - name: Tear down
        if: always()
        run: docker compose down -v
```

- [ ] **Step 2: 验证 workflow YAML 语法**

Run: `python3 -c "import yaml; yaml.safe_load(open('.github/workflows/ci.yml'))"`
Expected: 无输出，退出码 0。

- [ ] **Step 3: 提交**

```bash
git add .github/workflows/ci.yml
git commit -m "ci: 三 job 串联 workflow（unit -> integration -> e2e）

- unit: 每次 push + PR，纯 gmock 无依赖
- integration: PR 触发，docker-compose.test.yml 起基础设施
- e2e: PR to main + nightly，全套 docker-compose
- job 间 needs 串联，失败短路"
```

---

### Task 3: CMake CTest 集成 + dummy 测试

**Files:**
- Modify: `CMakeLists.txt`（根 CMake，追加 CTest 启用 + tests 子目录）
- Create: `tests/CMakeLists.txt`
- Create: `tests/dummy/CMakeLists.txt`
- Create: `tests/dummy/test_dummy_unit.cc`
- Create: `tests/dummy/test_dummy_integration.cc`
- Create: `tests/dummy/test_dummy_e2e.cc`

**Interfaces:**
- Produces: `dummy_unit_tests` / `dummy_integration_tests` / `dummy_e2e_tests` 三个可执行目标
- Produces: CTest labels `unit` / `integration` / `e2e` 注册机制

- [ ] **Step 1: 创建 dummy unit 测试**

Create `tests/dummy/test_dummy_unit.cc`:

```cpp
#include <gtest/gtest.h>

TEST(DummyUnit, Sanity) {
    EXPECT_EQ(1 + 1, 2);
}

TEST(DummyUnit, Placeholder) {
    SUCCEED() << "Phase 0a 基础设施就绪，等待 Phase 0b/1 填充真实单元测试";
}
```

- [ ] **Step 2: 创建 dummy integration 测试**

Create `tests/dummy/test_dummy_integration.cc`:

```cpp
#include <cstdlib>
#include <cstring>
#include <gtest/gtest.h>

static bool integration_enabled() {
    const char* e = std::getenv("DB_TEST");
    return e && std::strcmp(e, "1") == 0;
}

TEST(DummyIntegration, InfraReachable) {
    if (!integration_enabled()) GTEST_SKIP() << "DB_TEST!=1";
    SUCCEED() << "基础设施就绪，等待 Phase 1 填充真实 DAO 集成测试";
}
```

- [ ] **Step 3: 创建 dummy e2e 测试**

Create `tests/dummy/test_dummy_e2e.cc`:

```cpp
#include <cstdlib>
#include <cstring>
#include <gtest/gtest.h>

static bool e2e_enabled() {
    const char* e = std::getenv("E2E_TEST");
    return e && std::strcmp(e, "1") == 0;
}

TEST(DummyE2E, FullStackReachable) {
    if (!e2e_enabled()) GTEST_SKIP() << "E2E_TEST!=1";
    SUCCEED() << "全栈就绪，等待 Phase 1 填充真实 E2E 测试";
}
```

- [ ] **Step 4: 创建 dummy CMakeLists.txt**

Create `tests/dummy/CMakeLists.txt`:

```cmake
# dummy 测试：验证 CTest 三层 label 机制能跑通
cmake_minimum_required(VERSION 3.1.3)
project(dummy_tests)

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../../third/include)

# dummy unit
add_executable(dummy_unit_tests test_dummy_unit.cc)
target_link_libraries(dummy_unit_tests -lgtest -lgtest_main -lpthread)
gtest_discover_tests(dummy_unit_tests PROPERTIES LABELS "unit")

# dummy integration
add_executable(dummy_integration_tests test_dummy_integration.cc)
target_link_libraries(dummy_integration_tests -lgtest -lgtest_main -lpthread)
gtest_discover_tests(dummy_integration_tests PROPERTIES LABELS "integration")

# dummy e2e
add_executable(dummy_e2e_tests test_dummy_e2e.cc)
target_link_libraries(dummy_e2e_tests -lgtest -lgtest_main -lpthread)
gtest_discover_tests(dummy_e2e_tests PROPERTIES LABELS "e2e")
```

- [ ] **Step 5: 创建 tests/CMakeLists.txt**

Create `tests/CMakeLists.txt`:

```cmake
# tests 顶层 CMake：聚合所有测试子目录
enable_testing()

add_subdirectory(dummy)
# 以下子目录在后续 Task 中创建：
# add_subdirectory(fixtures)   # Task 4
# add_subdirectory(e2e)        # Task 5
# add_subdirectory(mocks)      # Task 6
```

- [ ] **Step 6: 修改根 CMakeLists.txt 接入 tests**

Modify `CMakeLists.txt`（在文件末尾追加）:

```cmake
# 5. 测试基础设施（CTest）
enable_testing()
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/tests)
```

完整的根 `CMakeLists.txt` 应为：

```cmake
# 1. 添加 cmake 版本说明
cmake_minimum_required(VERSION 3.1.3)
# 2. 声明工程名称
project(message_server)
# 3. 添加子目录
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/message)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/user)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/file)
# speech 子服务在 P4 已并入 media_server（位于 file/）；speech/ 目录已删除。
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/transmite)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/friend)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/chatsession)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/gateway)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/push)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/common/test)
# 4.
set(CMAKE_INSTALL_PREFIX ${CMAKE_CURRENT_BINARY_DIR})
# 5. 测试基础设施（CTest）
enable_testing()
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/tests)
```

- [ ] **Step 7: 构建并运行 dummy unit 测试**

Run:
```bash
mkdir -p build && cd build
cmake ..
make -j$(nproc) dummy_unit_tests
ctest -L unit --output-on-failure
```
Expected: `dummy_unit_tests` 编译成功；`ctest -L unit` 输出 2 个测试通过（Sanity + Placeholder）。

- [ ] **Step 8: 构建并运行 dummy integration 测试（不设环境变量，应 SKIP）**

Run:
```bash
cd build
make -j$(nproc) dummy_integration_tests
ctest -L integration --output-on-failure
```
Expected: `ctest -L integration` 输出 1 个测试 SKIPPED（DB_TEST 未设）。

- [ ] **Step 9: 提交**

```bash
git add CMakeLists.txt tests/CMakeLists.txt tests/dummy/
git commit -m "build(test): CTest 集成 + dummy 测试验证三层 label

- 根 CMakeLists.txt 追加 enable_testing() + tests 子目录
- tests/dummy/: 三个 dummy 测试（unit/integration/e2e）验证 CTest label 机制
- dummy integration/e2e 用环境变量门控（GTEST_SKIP）"
```

---

### Task 4: 共享 fixtures（db_fixture / mq_fixture / fake_closure / proto_helpers）

**Files:**
- Create: `tests/fixtures/fake_closure.hpp`
- Create: `tests/fixtures/proto_helpers.hpp`
- Create: `tests/fixtures/db_fixture.hpp`
- Create: `tests/fixtures/mq_fixture.hpp`
- Create: `tests/fixtures/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`（取消 fixtures 注释）

**Interfaces:**
- Produces: `FakeClosure`（protobuf Closure 假实现，记录 Run() 调用次数）
- Produces: `make_user()` / `make_message()` / `make_internal_message()`（protobuf 构造 helper）
- Produces: `DBFixture`（MySQL 测试基类， SetUp 时 TRUNCATE 表）
- Produces: `MQFixture`（RabbitMQ 测试基类，提供 publisher/subscriber + 唯一 queue）

- [ ] **Step 1: 创建 fake_closure.hpp**

Create `tests/fixtures/fake_closure.hpp`:

```cpp
#pragma once

/**
 * FakeClosure -- google::protobuf::Closure 的测试假实现
 * 记录 Run() 调用次数，供断言 "done->Run() 是否被调用"
 */
#include <atomic>
#include <google/protobuf/stubs/callback.h>

namespace chatnow {
namespace test {

class FakeClosure : public ::google::protobuf::Closure {
public:
    FakeClosure() : _run_count(0) {}

    void Run() override {
        _run_count.fetch_add(1, std::memory_order_relaxed);
    }

    int run_count() const {
        return _run_count.load(std::memory_order_relaxed);
    }

    bool was_run() const {
        return run_count() > 0;
    }

private:
    std::atomic<int> _run_count;
};

} // namespace test
} // namespace chatnow
```

- [ ] **Step 2: 创建 proto_helpers.hpp**

Create `tests/fixtures/proto_helpers.hpp`:

```cpp
#pragma once

/**
 * proto_helpers -- protobuf 对象构造 helper
 * make_user / make_message / make_internal_message 等
 * 供单元测试快速构造请求/响应对象
 */
#include <string>
#include <vector>
#include "common/types.pb.h"
#include "message/message_types.pb.h"
#include "message/message_internal.pb.h"

namespace chatnow {
namespace test {

inline ::chatnow::UserInfo make_user(const std::string& uid, const std::string& nickname) {
    ::chatnow::UserInfo u;
    u.set_user_id(uid);
    u.set_nickname(nickname);
    return u;
}

inline ::chatnow::MessageInfo make_message(int64_t message_id,
                                            const std::string& session_id,
                                            const std::string& sender_id,
                                            const std::string& content) {
    ::chatnow::MessageInfo m;
    m.set_message_id(message_id);
    m.set_chat_session_id(session_id);
    m.set_sender_id(sender_id);
    m.set_message_type(::chatnow::MessageType::TEXT);
    m.mutable_text_message()->set_content(content);
    return m;
}

inline ::chatnow::InternalMessage make_internal_message(
    int64_t message_id,
    const std::string& session_id,
    const std::string& sender_id,
    const std::vector<std::string>& member_ids,
    const std::string& content = "hello") {
    ::chatnow::InternalMessage im;
    auto* info = im.mutable_message();
    info->set_message_id(message_id);
    info->set_chat_session_id(session_id);
    info->set_sender_id(sender_id);
    info->set_message_type(::chatnow::MessageType::TEXT);
    info->mutable_text_message()->set_content(content);
    for (const auto& mid : member_ids) {
        im.add_member_id_list(mid);
    }
    return im;
}

} // namespace test
} // namespace chatnow
```

- [ ] **Step 3: 创建 db_fixture.hpp**

Create `tests/fixtures/db_fixture.hpp`:

```cpp
#pragma once

/**
 * DBFixture -- MySQL 测试基类
 * SetUpTestSuite: 建立连接
 * SetUp: TRUNCATE 相关表，保证每个测试数据隔离
 * 子类需 override truncate_tables() 返回要清空的表名列表
 */
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>
#include <gtest/gtest.h>
#include <odb/database.hxx>
#include <odb/mysql/database.hxx>
#include "message.hxx"
#include "user_timeline.hxx"

namespace chatnow {
namespace test {

inline std::shared_ptr<odb::core::database> make_test_db() {
    const char* host = std::getenv("DB_HOST");
    const char* user = std::getenv("DB_USER");
    const char* pass = std::getenv("DB_PASS");
    const char* name = std::getenv("DB_NAME");
    return std::make_shared<odb::mysql::database>(
        user ? user : "root",
        pass ? pass : "chatnow_test",
        name ? name : "chatnow_test",
        host ? host : "127.0.0.1",
        3306);
}

class DBFixture : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        if (!std::getenv("DB_TEST")) GTEST_SKIP() << "DB_TEST!=1";
        _db = make_test_db();
    }

    void SetUp() override {
        if (!_db) GTEST_SKIP() << "DB 未初始化";
        odb::transaction t(_db->begin());
        for (const auto& table : truncate_tables()) {
            _db->execute("TRUNCATE TABLE " + table);
        }
        t.commit();
    }

    virtual std::vector<std::string> truncate_tables() const {
        return {"message", "user_timeline", "chat_session_member"};
    }

    static inline std::shared_ptr<odb::core::database> _db;
};

} // namespace test
} // namespace chatnow
```

- [ ] **Step 4: 创建 mq_fixture.hpp**

Create `tests/fixtures/mq_fixture.hpp`:

```cpp
#pragma once

/**
 * MQFixture -- RabbitMQ 测试基类
 * SetUp: 建立 MQ 连接，提供 publisher + subscriber
 * 子测试用唯一 queue 名（test_<uuid>）避免互相干扰
 */
#include <atomic>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>
#include <gtest/gtest.h>
#include "mq/rabbitmq.hpp"

namespace chatnow {
namespace test {

inline std::string test_queue_name(const std::string& prefix) {
    static std::atomic<uint64_t> counter{0};
    return prefix + "_" + std::to_string(getpid()) + "_" +
           std::to_string(counter.fetch_add(1));
}

class MQFixture : public ::testing::Test {
protected:
    void SetUp() override {
        if (!std::getenv("MQ_TEST")) GTEST_SKIP() << "MQ_TEST!=1";
        const char* host = std::getenv("MQ_HOST");
        std::string mq_host = host ? host : "127.0.0.1";
        // MQClient 连接（具体构造取决于 rabbitmq.hpp 的 MQClient 接口）
        // Phase 0b 接口抽取后这里改为注入 IMQClient
        // 当前先占位，Phase 1 集成测试填充时完善
    }

    void TearDown() override {
        // 清理测试 queue（在子测试中声明）
    }

    std::shared_ptr<MQClient> _mq;
};

} // namespace test
} // namespace chatnow
```

> 注：`MQFixture` 的完整实现依赖 `MQClient` 的公开接口。Phase 0a 仅放骨架，Phase 1 集成测试任务中会根据当时的接口补完。这是允许的，因为 Phase 0a 的验收标准是 "dummy 测试 CI 绿"，不要求 fixtures 已被使用。

- [ ] **Step 5: 创建 fixtures/CMakeLists.txt**

Create `tests/fixtures/CMakeLists.txt`:

```cmake
# fixtures: header-only，不需要编译独立 target
# 各测试 target 通过 target_include_directories 引入此目录
```

- [ ] **Step 6: 修改 tests/CMakeLists.txt 取消 fixtures 注释**

Modify `tests/CMakeLists.txt`:

```cmake
# tests 顶层 CMake：聚合所有测试子目录
enable_testing()

add_subdirectory(dummy)
add_subdirectory(fixtures)
# 以下子目录在后续 Task 中创建：
# add_subdirectory(e2e)        # Task 5
# add_subdirectory(mocks)      # Task 6
```

- [ ] **Step 7: 验证 dummy 测试仍通过**

Run:
```bash
cd build
cmake ..
make -j$(nproc) dummy_unit_tests
ctest -L unit --output-on-failure
```
Expected: 2 个 dummy unit 测试通过（fixtures 是 header-only，不影响编译）。

- [ ] **Step 8: 提交**

```bash
git add tests/fixtures/ tests/CMakeLists.txt
git commit -m "test(fixtures): 共享测试 fixtures 骨架

- fake_closure.hpp: protobuf Closure 假实现，记录 Run() 调用次数
- proto_helpers.hpp: make_user/make_message/make_internal_message 构造 helper
- db_fixture.hpp: MySQL 测试基类，SetUp 时 TRUNCATE 表
- mq_fixture.hpp: RabbitMQ 测试基类骨架（Phase 1 集成测试时补完）"
```

---

### Task 5: E2E helpers 骨架（http_client / ws_client / test_users）

**Files:**
- Create: `tests/e2e/helpers/http_client.hpp`
- Create: `tests/e2e/helpers/ws_client.hpp`
- Create: `tests/e2e/helpers/test_users.hpp`
- Create: `tests/e2e/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`（取消 e2e 注释）

**Interfaces:**
- Produces: `HttpClient`（封装 cpp-httplib，带 JWT 注入）
- Produces: `WsClient`（封装 websocketpp，等通知）
- Produces: `TestUsers`（预置用户注册/登录 fixture）

- [ ] **Step 1: 创建 http_client.hpp**

Create `tests/e2e/helpers/http_client.hpp`:

```cpp
#pragma once

/**
 * HttpClient -- E2E 测试用 HTTP 客户端
 * 封装 cpp-httplib，自动注入 Authorization: Bearer <jwt>
 */
#include <httplib.h>
#include <json/json.h>
#include <string>

namespace chatnow {
namespace test {

class HttpClient {
public:
    explicit HttpClient(const std::string& base = "http://127.0.0.1:9000")
        : _cli(base) {}

    void set_token(const std::string& jwt) {
        _jwt = jwt;
    }

    Json::Value post(const std::string& path, const Json::Value& body) {
        httplib::Headers hdrs;
        if (!_jwt.empty()) {
            hdrs.emplace("Authorization", "Bearer " + _jwt);
        }
        Json::StreamWriterBuilder wb;
        std::string body_str = Json::writeString(wb, body);

        auto res = _cli.Post(path, hdrs, body_str, "application/json");
        if (!res) {
            throw std::runtime_error("HTTP POST 失败: " + path);
        }
        if (res->status != 200) {
            throw std::runtime_error("HTTP " + std::to_string(res->status) + ": " + path);
        }

        Json::CharReaderBuilder rb;
        Json::Value resp;
        std::string errs;
        std::istringstream s(res->body);
        if (!Json::parseFromStream(rb, s, &resp, &errs)) {
            throw std::runtime_error("JSON 解析失败: " + errs);
        }
        return resp;
    }

    Json::Value get(const std::string& path) {
        httplib::Headers hdrs;
        if (!_jwt.empty()) {
            hdrs.emplace("Authorization", "Bearer " + _jwt);
        }
        auto res = _cli.Get(path, hdrs);
        if (!res) {
            throw std::runtime_error("HTTP GET 失败: " + path);
        }
        Json::CharReaderBuilder rb;
        Json::Value resp;
        std::string errs;
        std::istringstream s(res->body);
        if (!Json::parseFromStream(rb, s, &resp, &errs)) {
            throw std::runtime_error("JSON 解析失败: " + errs);
        }
        return resp;
    }

private:
    httplib::Client _cli;
    std::string _jwt;
};

} // namespace test
} // namespace chatnow
```

- [ ] **Step 2: 创建 ws_client.hpp**

Create `tests/e2e/helpers/ws_client.hpp`:

```cpp
#pragma once

/**
 * WsClient -- E2E 测试用 WebSocket 客户端
 * 封装 websocketpp，阻塞等待指定类型通知
 */
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <websocketpp/config/asio_client.hpp>
#include <websocketpp/client.hpp>

namespace chatnow {
namespace test {

using ws_client = websocketpp::client<websocketpp::config::asio_tls_client>;
using message_ptr = ws_client::message_ptr;

class WsClient {
public:
    explicit WsClient(const std::string& url) : _url(url), _connected(false) {
        _client.init_asio();
        _client.set_open_handler([this](websocketpp::connection_hdl) {
            std::lock_guard<std::mutex> lk(_mtx);
            _connected = true;
            _cv.notify_all();
        });
        _client.set_message_handler([this](websocketpp::connection_hdl, message_ptr msg) {
            std::lock_guard<std::mutex> lk(_mtx);
            _messages.push_back(msg->get_payload());
            _cv.notify_all();
        });
    }

    void connect() {
        websocketpp::lib::error_code ec;
        auto con = _client.get_connection(_url, ec);
        if (ec) {
            throw std::runtime_error("WS 连接失败: " + ec.message());
        }
        _client.connect(con);
        _client.run();
    }

    bool wait_connected(int timeout_ms = 5000) {
        std::unique_lock<std::mutex> lk(_mtx);
        return _cv.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                            [this] { return _connected; });
    }

    bool wait_notify(int timeout_ms = 5000, std::string* out = nullptr) {
        std::unique_lock<std::mutex> lk(_mtx);
        bool got = _cv.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                                [this] { return !_messages.empty(); });
        if (got && out) {
            *out = _messages.front();
            _messages.pop_front();
        }
        return got;
    }

private:
    std::string _url;
    ws_client _client;
    std::atomic<bool> _connected;
    std::deque<std::string> _messages;
    std::mutex _mtx;
    std::condition_variable _cv;
};

} // namespace test
} // namespace chatnow
```

- [ ] **Step 3: 创建 test_users.hpp**

Create `tests/e2e/helpers/test_users.hpp`:

```cpp
#pragma once

/**
 * TestUsers -- E2E 测试预置用户 fixture
 * 构造时注册 u1/u2/u3 并登录拿 JWT
 * 依赖 HttpClient（Task 5 Step 1）
 */
#include <map>
#include <string>
#include "http_client.hpp"

namespace chatnow {
namespace test {

struct UserInfo {
    std::string uid;
    std::string nickname;
    std::string jwt;
    std::string session_id;
};

class TestUsers {
public:
    explicit TestUsers(HttpClient& http) : _http(http) {
        // Phase 1 E2E 测试任务中填充注册/登录逻辑
        // 当前仅提供骨架，E2E 测试执行时完善
    }

    const UserInfo& operator[](const std::string& uid) const {
        return _users.at(uid);
    }

    bool has(const std::string& uid) const {
        return _users.count(uid) > 0;
    }

private:
    HttpClient& _http;
    std::map<std::string, UserInfo> _users;
};

} // namespace test
} // namespace chatnow
```

- [ ] **Step 4: 创建 e2e/CMakeLists.txt**

Create `tests/e2e/CMakeLists.txt`:

```cmake
# E2E 测试骨架
# Phase 0a: 仅创建目录结构，真实 E2E 测试在 Phase 1 添加
# Phase 1 会在本目录添加 test_message_pipeline.cc 等文件
```

- [ ] **Step 5: 修改 tests/CMakeLists.txt 取消 e2e 注释**

Modify `tests/CMakeLists.txt`:

```cmake
# tests 顶层 CMake：聚合所有测试子目录
enable_testing()

add_subdirectory(dummy)
add_subdirectory(fixtures)
add_subdirectory(e2e)
# 以下子目录在后续 Task 中创建：
# add_subdirectory(mocks)      # Task 6
```

- [ ] **Step 6: 验证 dummy 测试仍通过**

Run:
```bash
cd build
cmake ..
make -j$(nproc) dummy_unit_tests
ctest -L unit --output-on-failure
```
Expected: 2 个 dummy unit 测试通过（e2e helpers 是 header-only，不影响编译）。

- [ ] **Step 7: 提交**

```bash
git add tests/e2e/ tests/CMakeLists.txt
git commit -m "test(e2e): E2E helpers 骨架

- http_client.hpp: 封装 cpp-httplib，自动注入 JWT
- ws_client.hpp: 封装 websocketpp，阻塞等待通知
- test_users.hpp: 预置用户注册/登录 fixture 骨架（Phase 1 填充）
- 真实 E2E 测试在 Phase 1 添加"
```

---

### Task 6: mocks 目录骨架

**Files:**
- Create: `tests/mocks/CMakeLists.txt`
- Create: `tests/mocks/README.md`
- Modify: `tests/CMakeLists.txt`（取消 mocks 注释）

**Interfaces:**
- Produces: `tests/mocks/` 目录（Phase 0b 接口抽取后在此创建 mock_*.hpp）

- [ ] **Step 1: 创建 mocks/CMakeLists.txt**

Create `tests/mocks/CMakeLists.txt`:

```cmake
# mocks: header-only gmock 类
# Phase 0b 接口抽取后在此目录添加 mock_*.hpp
# 各测试 target 通过 target_include_directories 引入此目录
```

- [ ] **Step 2: 创建 mocks/README.md**

Create `tests/mocks/README.md`:

```markdown
# tests/mocks/

gmock mock 类存放目录。每个 mock 类对应一个 `i_*.hpp` 接口。

## 命名约定

- `mock_<interface_name>.hpp`，例如 `mock_publisher.hpp` 对应 `mq/i_publisher.hpp`
- 仅 include 接口头 + `<gmock/gmock.h>`，不 include 具体实现头

## 添加时机

Phase 0b（接口抽取）完成后，每个抽取的接口在此添加对应 mock。
```

- [ ] **Step 3: 修改 tests/CMakeLists.txt 取消 mocks 注释**

Modify `tests/CMakeLists.txt`:

```cmake
# tests 顶层 CMake：聚合所有测试子目录
enable_testing()

add_subdirectory(dummy)
add_subdirectory(fixtures)
add_subdirectory(e2e)
add_subdirectory(mocks)
```

- [ ] **Step 4: 验证完整构建**

Run:
```bash
cd build
cmake ..
make -j$(nproc) dummy_unit_tests dummy_integration_tests dummy_e2e_tests
ctest -L unit --output-on-failure
ctest -L integration --output-on-failure
```
Expected: unit 2 个通过；integration 1 个 SKIPPED（DB_TEST 未设）。

- [ ] **Step 5: 提交**

```bash
git add tests/mocks/ tests/CMakeLists.txt
git commit -m "test(mocks): gmock mock 类目录骨架

- tests/mocks/: Phase 0b 接口抽取后在此添加 mock_*.hpp
- 命名约定: mock_<interface_name>.hpp 对应 i_<interface_name>.hpp"
```

---

### Task 7: 验收 - 本地完整跑通 + 模拟 CI 流程

**Files:**
- 无新增/修改

- [ ] **Step 1: 本地跑 unit 测试**

Run:
```bash
cd build
ctest -L unit --output-on-failure
```
Expected: 2 个 dummy unit 测试通过。

- [ ] **Step 2: 本地起基础设施跑 integration dummy**

Run:
```bash
docker compose -f docker/docker-compose.test.yml up -d
./scripts/wait_for_infra.sh
cd build
DB_TEST=1 ctest -L integration --output-on-failure
docker compose -f docker/docker-compose.test.yml down -v
```
Expected: `wait_for_infra.sh` 输出 "所有基础设施就绪"；integration dummy 测试通过（不再 SKIP）。

- [ ] **Step 3: 验证 git log 完整**

Run: `git log --oneline -10`
Expected: 看到 Task 1-6 的 6 个提交，消息清晰。

- [ ] **Step 4: 验证目录结构完整**

Run: `find tests/ docker/docker-compose.test.yml scripts/wait_for_*.sh .github/workflows/ci.yml -type f | sort`
Expected: 输出包含所有本 plan 创建的文件。

- [ ] **Step 5: 提交验收标记（可选 tag）**

```bash
git tag phase-0a-complete
git log --oneline -1
```
Expected: tag `phase-0a-complete` 创建成功。

---

## 验收标准

Phase 0a 完成后应满足：

1. **CI workflow 绿** - push 到任意分支时 `unit` job 通过（dummy 测试跑通）
2. **本地可复现** - `docker compose -f docker/docker-compose.test.yml up -d` + `ctest -L integration` 能跑通 dummy integration 测试
3. **目录骨架就位** - `tests/{dummy,fixtures,e2e,mocks}/` 四个子目录存在且有 CMakeLists.txt
4. **CTest labels 生效** - `ctest -L unit` / `ctest -L integration` / `ctest -L e2e` 能正确过滤
5. **不破坏现有测试** - `common/test/` 的 14 个单元测试仍编译通过（根 CMakeLists.txt 只新增 `enable_testing()` + `add_subdirectory(tests)`，不改现有目标）

## 下一步

Phase 0a 完成后，进入 **Phase 0b: 接口抽取**（独立 plan）：
- 为 transmite + message 服务的依赖抽 `i_*.hpp` 接口
- ServiceImpl 改持有 `shared_ptr<I*>`
- 在 `tests/mocks/` 添加对应 `mock_*.hpp`
- 验收：现有 `common/test/` 仍通过 + 服务仍可编译启动

之后是 **Phase 1: 核心消息链路测试**（独立 plan）：
- transmite/message 单元测试（10 个文件）
- message DAO 集成测试（5 个文件）
- media DAO 集成测试补齐（3 个文件）
- E2E 测试（3 个文件）
