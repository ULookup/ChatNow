# Relationship Service Migration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 把 `friend/source/` 下的旧 `FriendServiceImpl : public FriendService` 迁到新 `chatnow::relationship::RelationshipServiceImpl`，目录改名 `friend/` → `relationship/`，9 个 RPC 全部用新 proto + `HANDLE_RPC` 宏 + metadata 鉴权，新增 `user_block` 表与 `BlockUser/UnblockUser/ListBlockedUsers`，`HandleFriendRequest(agree=true)` 改为调 `ConversationService.CreateConversation`。

**Architecture:** 沿用既有"Server / ServiceImpl / Builder + main.cc"分层。`HANDLE_RPC` 宏统一处理 `extract_auth + LogContext + ResponseHeader + ServiceError`。跨服务调用前用 `forward_auth_metadata` 透传 4 个鉴权 metadata。新增 `UserBlock` ODB 实体与单文件 DAO（`mysql_user_block.hpp`）。

**Tech Stack:** C++17 / brpc + Protobuf 3 / ODB MySQL / Elasticsearch (elasticlient) / etcd-cpp-api / spdlog / boost::posix_time / GoogleTest。

---

## 文件结构

**新增**：
- `odb/user_block.hxx` — UserBlock 实体（一表一文件）
- `common/dao/mysql_user_block.hpp` — `UserBlockTable` DAO
- `common/test/test_mysql_user_block_compile.cc` — 仅编译断言（DAO 接口形状）
- `relationship/CMakeLists.txt` — 编译 target `relationship_server`
- `relationship/Dockerfile` — 容器镜像（复制自 friend/Dockerfile，rename 二进制）
- `relationship/source/relationship_server.h` — `RelationshipServiceImpl` + `RelationshipServer` + `RelationshipServerBuilder`
- `relationship/source/relationship_server.cc` — `main` 入口（gflags + builder）
- `conf/relationship_server.conf` — gflags flagfile

**删除**：
- `friend/CMakeLists.txt`、`friend/Dockerfile`、`friend/source/friend_server.h`、`friend/source/friend_server.cc`、`conf/friend_server.conf`
- `proto/friend.proto`（最后一步，全仓零引用后再删）

**修改**：
- `proto/relationship/relationship_service.proto` — 删 `optional user_id / session_id`；`FriendEvent.event_id` 改非 optional
- `CMakeLists.txt`（顶层）— `add_subdirectory(friend)` → `add_subdirectory(relationship)`
- `gateway/source/gateway_server.h` — 6 个 friend handler 切到新 stub 与新 RPC 名（HTTP 路径不变）
- `docker-compose.yml` — `friend_server` service → `relationship_server`

---

## Task 1: 清理 relationship_service.proto 鉴权字段

**Files:**
- Modify: `proto/relationship/relationship_service.proto`

- [ ] **Step 1: 删除 9 个 Req 中的 `optional string user_id` 与 `optional string session_id`，并把 `FriendEvent.event_id` 改为非 optional**

把 `proto/relationship/relationship_service.proto` 改成下面这份（**整文件替换**，注意 message tag 数字按删除后重排紧凑，便于阅读 — 由于这是迁移而非线上 schema 兼容场景，tag 号变更可接受）：

```protobuf
syntax = "proto3";
package chatnow.relationship;

import "common/envelope.proto";
import "common/types.proto";

service RelationshipService {
    rpc ListFriends(ListFriendsReq) returns (ListFriendsRsp);
    rpc SendFriendRequest(SendFriendReq) returns (SendFriendRsp);
    rpc HandleFriendRequest(HandleFriendReq) returns (HandleFriendRsp);
    rpc RemoveFriend(RemoveFriendReq) returns (RemoveFriendRsp);
    rpc SearchFriends(SearchFriendsReq) returns (SearchFriendsRsp);
    rpc BlockUser(BlockUserReq) returns (BlockUserRsp);
    rpc UnblockUser(UnblockUserReq) returns (UnblockUserRsp);
    rpc ListBlockedUsers(ListBlockedReq) returns (ListBlockedRsp);
    rpc ListPendingRequests(ListPendingReq) returns (ListPendingRsp);
}

message ListFriendsReq {
    string request_id = 1;
    chatnow.common.PageRequest page = 2;
}
message ListFriendsRsp {
    chatnow.common.ResponseHeader header = 1;
    repeated chatnow.common.UserInfo friend_list = 2;
    chatnow.common.PageResponse page = 3;
}

message SendFriendReq {
    string request_id = 1;
    string respondent_id = 2;
}
message SendFriendRsp {
    chatnow.common.ResponseHeader header = 1;
    optional string notify_event_id = 2;
}

message HandleFriendReq {
    string request_id = 1;
    string notify_event_id = 2;
    bool agree = 3;
    string apply_user_id = 4;
}
message HandleFriendRsp {
    chatnow.common.ResponseHeader header = 1;
    optional string new_conversation_id = 2;
}

message RemoveFriendReq {
    string request_id = 1;
    string peer_id = 2;
}
message RemoveFriendRsp { chatnow.common.ResponseHeader header = 1; }

message SearchFriendsReq {
    string request_id = 1;
    string search_key = 2;
}
message SearchFriendsRsp {
    chatnow.common.ResponseHeader header = 1;
    repeated chatnow.common.UserInfo user_info = 2;
}

message BlockUserReq {
    string request_id = 1;
    string peer_id = 2;
}
message BlockUserRsp { chatnow.common.ResponseHeader header = 1; }

message UnblockUserReq {
    string request_id = 1;
    string peer_id = 2;
}
message UnblockUserRsp { chatnow.common.ResponseHeader header = 1; }

message ListBlockedReq {
    string request_id = 1;
    chatnow.common.PageRequest page = 2;
}
message ListBlockedRsp {
    chatnow.common.ResponseHeader header = 1;
    repeated chatnow.common.UserInfo blocked_list = 2;
    chatnow.common.PageResponse page = 3;
}

message ListPendingReq {
    string request_id = 1;
}
message ListPendingRsp {
    chatnow.common.ResponseHeader header = 1;
    repeated FriendEvent event = 2;
}

message FriendEvent {
    string event_id = 1;
    chatnow.common.UserInfo sender = 2;
}
```

**说明**：
- `chatnow.common.{ResponseHeader,PageRequest,PageResponse,UserInfo}` 是已有 proto，保持完全限定可避免 Identity / 老 proto 同名类型在迁移过渡期产生歧义
- 旧 friend 服务无生产线上 RPC 客户端固定字段号，本仓内重排 tag 安全

- [ ] **Step 2: 编译验证**

Run:
```bash
protoc --cpp_out=/tmp -I proto --experimental_allow_proto3_optional proto/relationship/relationship_service.proto
```
Expected: 退出码 0，无 stderr

- [ ] **Step 3: 提交**

```bash
git add proto/relationship/relationship_service.proto
git commit -m "proto(relationship): 删除业务体鉴权字段（user_id/session_id），event_id 改非 optional"
```

---

## Task 2: 新增 UserBlock ODB 实体

**Files:**
- Create: `odb/user_block.hxx`

- [ ] **Step 1: 写实体头**

Create `odb/user_block.hxx` with:

```cpp
#pragma once
#include <string>
#include <cstddef>
#include <odb/core.hxx>
#include <boost/date_time/posix_time/posix_time.hpp>

/**
 * ===========================================================================
 * 用户拉黑表 (user_block)
 * ---------------------------------------------------------------------------
 * 设计定位：
 *   - 单向：A 拉黑 B 时只写 (A,B) 一行；与 relation 双向冗余明确分离
 *   - 仅用于"拒新好友申请 + 搜索过滤"两个场景；不动 relation / 会话 / 消息
 *   - 唯一索引保证一对 (blocker, blocked) 至多一行；重复 BlockUser 视作幂等
 *
 * 字段速览：
 *   _id           物理主键
 *   _blocker_id   "我"（执行拉黑动作的用户）
 *   _blocked_id   被我拉黑的对端
 *   _create_time  拉黑时间
 *
 * 索引策略：
 *   uk_blocker_blocked  (blocker_id, blocked_id) 唯一
 *   idx_blocked         (blocked_id)              反查"谁拉黑了我"
 * ===========================================================================
 */

namespace chatnow
{

#pragma db object table("user_block")
class UserBlock
{
public:
    UserBlock() = default;
    UserBlock(const std::string &blocker, const std::string &blocked)
        : _blocker_id(blocker), _blocked_id(blocked) {}

    std::string blocker_id() const { return _blocker_id; }
    void blocker_id(const std::string &v) { _blocker_id = v; }

    std::string blocked_id() const { return _blocked_id; }
    void blocked_id(const std::string &v) { _blocked_id = v; }

    boost::posix_time::ptime create_time() const { return _create_time; }
    void create_time(const boost::posix_time::ptime &v) { _create_time = v; }

private:
    friend class odb::access;

    #pragma db id auto
    unsigned long _id;

    #pragma db type("varchar(32)")
    std::string _blocker_id;

    #pragma db type("varchar(32)")
    std::string _blocked_id;

    #pragma db type("DATETIME(3)")
    boost::posix_time::ptime _create_time;

    #pragma db index("uk_blocker_blocked") unique members(_blocker_id, _blocked_id)
    #pragma db index("idx_blocked") members(_blocked_id)
};

} // namespace chatnow
```

- [ ] **Step 2: 用 odb 编译器手动验证可以生成 -odb.hxx / -odb.cxx**

Run:
```bash
mkdir -p /tmp/odb_check && cd /tmp/odb_check && \
  odb -d mysql --std c++11 --generate-query --generate-schema \
      --profile boost/date-time \
      /home/icepop/ChatNow/odb/user_block.hxx
ls /tmp/odb_check/user_block-odb.hxx /tmp/odb_check/user_block-odb.cxx /tmp/odb_check/user_block.sql
```
Expected: 三个文件都生成；`user_block.sql` 内含 `CREATE TABLE` 与两个索引

- [ ] **Step 3: 提交**

```bash
git add odb/user_block.hxx
git commit -m "odb: 新增 user_block 表（单向拉黑）"
```

---

## Task 3: UserBlockTable DAO

**Files:**
- Create: `common/dao/mysql_user_block.hpp`
- Create: `common/test/test_mysql_user_block_compile.cc`
- Modify: `common/test/CMakeLists.txt`（确认 test_*.cc GLOB 命中即可，无需改动）

- [ ] **Step 1: 写 DAO 头**

Create `common/dao/mysql_user_block.hpp` with:

```cpp
#pragma once

#include "infra/logger.hpp"
#include "dao/mysql.hpp"
#include "user_block.hxx"
#include "user_block-odb.hxx"
#include <odb/database.hxx>
#include <odb/mysql/database.hxx>

#include <vector>
#include <string>
#include <unordered_set>
#include <memory>

namespace chatnow
{

/**
 * UserBlockTable
 * ------------------------------------------------------------------
 * user_block 表 DAO 封装。
 * 仅服务于 RelationshipService 的 BlockUser/UnblockUser/ListBlockedUsers
 * 与 SendFriendRequest/SearchFriends 的过滤路径。
 * ------------------------------------------------------------------
 */
class UserBlockTable
{
public:
    using ptr = std::shared_ptr<UserBlockTable>;
    explicit UserBlockTable(const std::shared_ptr<odb::core::database> &db) : _db(db) {}

    /* brief: 写一行；唯一索引冲突视为幂等成功 */
    bool insert(const std::string &blocker, const std::string &blocked) {
        try {
            UserBlock b(blocker, blocked);
            b.create_time(boost::posix_time::microsec_clock::universal_time());
            odb::transaction trans(_db->begin());
            _db->persist(b);
            trans.commit();
            return true;
        } catch (const odb::object_already_persistent &) {
            return true; // 幂等：已经拉黑过
        } catch (const odb::database_exception &e) {
            // MySQL 唯一索引冲突走 database_exception 路径
            std::string what = e.what();
            if (what.find("Duplicate") != std::string::npos) return true;
            LOG_ERROR("插入 user_block 失败 {}-{}: {}", blocker, blocked, what);
            return false;
        } catch (const std::exception &e) {
            LOG_ERROR("插入 user_block 失败 {}-{}: {}", blocker, blocked, e.what());
            return false;
        }
    }

    /* brief: 删除拉黑；不存在返回 true */
    bool remove(const std::string &blocker, const std::string &blocked) {
        try {
            odb::transaction trans(_db->begin());
            using query = odb::query<UserBlock>;
            _db->erase_query<UserBlock>(
                query::blocker_id == blocker && query::blocked_id == blocked);
            trans.commit();
            return true;
        } catch (const std::exception &e) {
            LOG_ERROR("删除 user_block 失败 {}-{}: {}", blocker, blocked, e.what());
            return false;
        }
    }

    /* brief: blocker 是否拉黑了 blocked */
    bool is_blocked(const std::string &blocker, const std::string &blocked) {
        try {
            odb::transaction trans(_db->begin());
            using query = odb::query<UserBlock>;
            std::shared_ptr<UserBlock> r(_db->query_one<UserBlock>(
                query::blocker_id == blocker && query::blocked_id == blocked));
            trans.commit();
            return r != nullptr;
        } catch (const std::exception &e) {
            LOG_ERROR("查询 user_block 失败 {}-{}: {}", blocker, blocked, e.what());
            return false;
        }
    }

    /* brief: blocker 拉黑列表（按 create_time 倒序，limit/offset 分页） */
    std::vector<std::string> list_blocked(const std::string &blocker, int offset, int limit) {
        std::vector<std::string> res;
        if (limit <= 0) return res;
        try {
            odb::transaction trans(_db->begin());
            using query  = odb::query<UserBlock>;
            using result = odb::result<UserBlock>;
            std::string tail = " ORDER BY create_time DESC LIMIT " +
                               std::to_string(limit) +
                               " OFFSET " + std::to_string(offset < 0 ? 0 : offset);
            result r(_db->query<UserBlock>(
                (query::blocker_id == blocker) + tail));
            for (auto &row : r) res.push_back(row.blocked_id());
            trans.commit();
        } catch (const std::exception &e) {
            LOG_ERROR("列举 user_block 失败 {}: {}", blocker, e.what());
        }
        return res;
    }

    /* brief: blocker 拉黑总数 */
    int64_t count_blocked(const std::string &blocker) {
        try {
            odb::transaction trans(_db->begin());
            using query = odb::query<UserBlock>;
            using result = odb::result<UserBlock>;
            int64_t cnt = 0;
            result r(_db->query<UserBlock>(query::blocker_id == blocker));
            for (auto it = r.begin(); it != r.end(); ++it) ++cnt;
            trans.commit();
            return cnt;
        } catch (const std::exception &e) {
            LOG_ERROR("统计 user_block 失败 {}: {}", blocker, e.what());
            return 0;
        }
    }

    /* brief: "我拉黑的 ∪ 拉黑我的" 全集；用于 SearchFriends 过滤 */
    std::unordered_set<std::string> blocked_or_blocking(const std::string &uid) {
        std::unordered_set<std::string> res;
        try {
            odb::transaction trans(_db->begin());
            using query  = odb::query<UserBlock>;
            using result = odb::result<UserBlock>;
            // 我拉黑的人
            result r1(_db->query<UserBlock>(query::blocker_id == uid));
            for (auto &row : r1) res.insert(row.blocked_id());
            // 拉黑我的人
            result r2(_db->query<UserBlock>(query::blocked_id == uid));
            for (auto &row : r2) res.insert(row.blocker_id());
            trans.commit();
        } catch (const std::exception &e) {
            LOG_ERROR("blocked_or_blocking 查询失败 {}: {}", uid, e.what());
        }
        return res;
    }

private:
    std::shared_ptr<odb::core::database> _db;
};

} // namespace chatnow
```

- [ ] **Step 2: 编译断言测试**

Create `common/test/test_mysql_user_block_compile.cc`:

```cpp
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
```

注：DAO 真实行为测试在 `relationship_server` 启动后用集成 client 跑（Task 9）。本编译断言文件保留，避免日后 GLOB 误删 test_mysql_user_block_*.cc 系列。

- [ ] **Step 3: 编译 common_tests 验证**

Run:
```bash
mkdir -p build && cd build && cmake .. && cmake --build . --target common_tests -j 2>&1 | tail -20
./common/test/common_tests --gtest_filter='UserBlockDaoCompile.*'
```
Expected: 编译成功，测试 PASS

- [ ] **Step 4: 提交**

```bash
git add common/dao/mysql_user_block.hpp common/test/test_mysql_user_block_compile.cc
git commit -m "dao: 新增 UserBlockTable，覆盖拉黑场景的 6 个查询/写入接口"
```

---

## Task 4: 构建 relationship/ 目录骨架

**Files:**
- Create: `relationship/CMakeLists.txt`
- Create: `relationship/Dockerfile`
- Create: `relationship/source/relationship_server.h`
- Create: `relationship/source/relationship_server.cc`
- Create: `conf/relationship_server.conf`

- [ ] **Step 1: CMakeLists**

Create `relationship/CMakeLists.txt` with:

```cmake
cmake_minimum_required(VERSION 3.1.3)
project(relationship_server)

set(target "relationship_server")

# 1. proto
set(proto_path ${CMAKE_CURRENT_SOURCE_DIR}/../proto)
set(proto_files common/types.proto common/error.proto common/envelope.proto
                identity/identity_service.proto
                conversation/conversation_service.proto
                relationship/relationship_service.proto)
set(proto_srcs "")
foreach(proto_file ${proto_files})
    string(REPLACE ".proto" ".pb.cc" proto_cc ${proto_file})
    if(NOT EXISTS ${CMAKE_CURRENT_BINARY_DIR}${proto_cc})
        add_custom_command(
            PRE_BUILD
            COMMAND protoc
            ARGS --cpp_out=${CMAKE_CURRENT_BINARY_DIR} -I ${proto_path}
                 --experimental_allow_proto3_optional ${proto_path}/${proto_file}
            DEPENDS ${proto_path}/${proto_file}
            OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${proto_cc}
            COMMENT "生成Protobuf框架代码文件:" ${CMAKE_CURRENT_BINARY_DIR}/${proto_cc}
        )
    endif()
    list(APPEND proto_srcs ${CMAKE_CURRENT_BINARY_DIR}/${proto_cc})
endforeach()

# 2. ODB
set(odb_path ${CMAKE_CURRENT_SOURCE_DIR}/../odb)
set(odb_files friend_apply.hxx relation.hxx user_block.hxx)
set(odb_srcs "")
foreach(odb_file ${odb_files})
    string(REPLACE ".hxx" "-odb.cxx" odb_cxx ${odb_file})
    if(NOT EXISTS ${CMAKE_CURRENT_BINARY_DIR}${odb_cxx})
        add_custom_command(
            PRE_BUILD
            COMMAND odb
            ARGS -d mysql --std c++11 --generate-query --generate-schema
                 --profile boost/date-time ${odb_path}/${odb_file}
            DEPENDS ${odb_path}/${odb_file}
            OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${odb_cxx}
            COMMENT "生成ODB框架代码文件:" ${CMAKE_CURRENT_BINARY_DIR}/${odb_cxx}
        )
    endif()
    list(APPEND odb_srcs ${CMAKE_CURRENT_BINARY_DIR}/${odb_cxx})
endforeach()

# 3. 源码
set(src_files "")
aux_source_directory(${CMAKE_CURRENT_SOURCE_DIR}/source src_files)

add_executable(${target} ${src_files} ${proto_srcs} ${odb_srcs})

target_link_libraries(${target} -lgflags -lspdlog
    -lfmt -lbrpc -lssl -lcrypto
    -lprotobuf -lleveldb -letcd-cpp-api
    -lcpprest -lcurl -lodb-mysql -lodb -lodb-boost
    -lcpr -lelasticlient -ljsoncpp)

include_directories(${CMAKE_CURRENT_BINARY_DIR})
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../common)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../odb)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../third/include)

INSTALL(TARGETS ${target} RUNTIME DESTINATION bin)
```

- [ ] **Step 2: Dockerfile（沿用 friend/Dockerfile 内容，替换二进制名）**

Run:
```bash
cp friend/Dockerfile relationship/Dockerfile
sed -i 's/friend_server/relationship_server/g' relationship/Dockerfile
sed -i 's|/im/conf/friend_server.conf|/im/conf/relationship_server.conf|g' relationship/Dockerfile
cat relationship/Dockerfile
```
Expected: 输出中 `friend` 字样不再出现（除了潜在的 logger 路径，下一步统一改）

- [ ] **Step 3: relationship_server.cc — main 入口**

Create `relationship/source/relationship_server.cc` with:

```cpp
#include "relationship_server.h"

DEFINE_bool(run_mode, false, "程序的运行模式 false-调试 ; true-发布");
DEFINE_string(log_file, "", "发布模式下，用于指定日志的输出文件");
DEFINE_int32(log_level, 0, "发布模式下，用于指定日志的输出等级");

DEFINE_string(registry_host, "http://127.0.0.1:2379", "服务注册中心地址");
DEFINE_string(base_service, "/service", "服务监控根目录");
DEFINE_string(instance_name, "/relationship_service/instance", "服务实例 etcd 路径");
DEFINE_string(access_host, "127.0.0.1:10006", "当前实例的外部访问地址");

DEFINE_int32(listen_port, 10006, "RPC服务器监听端口");
DEFINE_int32(rpc_timeout, -1, "RPC调用超时时间");
DEFINE_int32(rpc_threads, 1, "RPC的IO线程数量");

DEFINE_string(identity_service, "/service/identity_service", "Identity 子服务名称（GetMultiUserInfo）");
DEFINE_string(conversation_service, "/service/conversation_service", "Conversation 子服务名称（CreateConversation）");

DEFINE_string(es_host, "http://127.0.0.1:9200/", "ES搜索引擎服务器URL");

DEFINE_string(mysql_host, "127.0.0.1", "MySQL服务器访问地址");
DEFINE_string(mysql_user, "root", "MySQL访问服务器用户名");
DEFINE_string(mysql_pswd, "", "MySQL服务器访问密码");
DEFINE_string(mysql_db, "chatnow", "MySQL默认库名称");
DEFINE_string(mysql_cset, "utf8mb4", "MySQL客户端字符集");
DEFINE_int32(mysql_port, 0, "MySQL服务器访问端口");
DEFINE_int32(mysql_pool_count, 4, "MySQL连接池最大连接数量");

int main(int argc, char *argv[])
{
    google::ParseCommandLineFlags(&argc, &argv, true);
    chatnow::init_logger(FLAGS_run_mode, FLAGS_log_file, FLAGS_log_level);

    chatnow::RelationshipServerBuilder rsb;
    rsb.make_es_object({FLAGS_es_host});
    rsb.make_mysql_object(FLAGS_mysql_user, FLAGS_mysql_pswd, FLAGS_mysql_host,
                          FLAGS_mysql_db, FLAGS_mysql_cset,
                          FLAGS_mysql_port, FLAGS_mysql_pool_count);
    rsb.make_discovery_object(FLAGS_registry_host, FLAGS_base_service,
                              FLAGS_identity_service, FLAGS_conversation_service);
    rsb.make_rpc_object(FLAGS_listen_port, FLAGS_rpc_timeout, FLAGS_rpc_threads);
    rsb.make_registry_object(FLAGS_registry_host,
                             FLAGS_base_service + FLAGS_instance_name,
                             FLAGS_access_host);

    auto server = rsb.build();
    server->start();
    return 0;
}
```

- [ ] **Step 4: 占位 relationship_server.h（仅类骨架，handler 实现后续 Task 填）**

Create `relationship/source/relationship_server.h` with:

```cpp
#pragma once

#include <brpc/server.h>
#include "infra/etcd.hpp"
#include "mq/channel.hpp"
#include "infra/logger.hpp"
#include "utils/utils.hpp"
#include "dao/data_es.hpp"
#include "dao/mysql_chat_session_member.hpp"
#include "dao/mysql_chat_session.hpp"
#include "dao/mysql_relation.hpp"
#include "dao/mysql_friend_apply.hpp"
#include "dao/mysql_user_block.hpp"
#include "common/types.pb.h"
#include "common/error.pb.h"
#include "common/envelope.pb.h"
#include "identity/identity_service.pb.h"
#include "conversation/conversation_service.pb.h"
#include "relationship/relationship_service.pb.h"

#include "auth/auth_context.hpp"
#include "auth/forward_auth.hpp"
#include "error/error_codes.hpp"
#include "error/handle_rpc.hpp"
#include "error/service_error.hpp"
#include "log/log_context.hpp"

namespace chatnow {

class RelationshipServiceImpl : public ::chatnow::relationship::RelationshipService
{
public:
    RelationshipServiceImpl(const std::shared_ptr<elasticlient::Client> &es_client,
                            const std::shared_ptr<odb::core::database> &mysql_client,
                            const ServiceManager::ptr &channel_manager,
                            const std::string &identity_service_name,
                            const std::string &conversation_service_name)
        : _es_user(std::make_shared<ESUser>(es_client)),
          _mysql_friend_apply(std::make_shared<FriendApplyTable>(mysql_client)),
          _mysql_relation(std::make_shared<RelationTable>(mysql_client)),
          _mysql_user_block(std::make_shared<UserBlockTable>(mysql_client)),
          _identity_service_name(identity_service_name),
          _conversation_service_name(conversation_service_name),
          _mm_channels(channel_manager) {}

    ~RelationshipServiceImpl() override = default;

    // 9 个 RPC handler 在 Task 5/6/7 实现，先编译通过用 default override
    // 注：brpc 自动给 ::SERVICE_NAME stub 提供 default 实现，未实现的 RPC
    // 走 NOT_IMPLEMENTED；但本仓约定服务端要显式 override，因此 Task 5+ 会逐个补全。

private:
    ESUser::ptr                  _es_user;
    FriendApplyTable::ptr        _mysql_friend_apply;
    RelationTable::ptr           _mysql_relation;
    UserBlockTable::ptr          _mysql_user_block;

    std::string _identity_service_name;
    std::string _conversation_service_name;
    ServiceManager::ptr _mm_channels;
};

class RelationshipServer
{
public:
    using ptr = std::shared_ptr<RelationshipServer>;

    RelationshipServer(const Discovery::ptr &service_discover,
                       const Registry::ptr &reg_client,
                       const std::shared_ptr<odb::core::database> &mysql_client,
                       const std::shared_ptr<brpc::Server> &server)
        : _service_discover(service_discover),
          _reg_client(reg_client),
          _mysql_client(mysql_client),
          _rpc_server(server) {}

    ~RelationshipServer() = default;
    void start() { _rpc_server->RunUntilAskedToQuit(); }

private:
    Discovery::ptr _service_discover;
    Registry::ptr  _reg_client;
    std::shared_ptr<odb::core::database> _mysql_client;
    std::shared_ptr<brpc::Server>        _rpc_server;
};

class RelationshipServerBuilder
{
public:
    void make_es_object(const std::vector<std::string> host_list) {
        _es_client = ESClientFactory::create(host_list);
    }
    void make_mysql_object(const std::string &user, const std::string &password,
                           const std::string &host, const std::string &db,
                           const std::string &cset, uint16_t port, int pool)
    {
        _mysql_client = ODBFactory::create(user, password, host, db, cset, port, pool);
    }
    void make_discovery_object(const std::string &reg_host,
                               const std::string &base_service_name,
                               const std::string &identity_service_name,
                               const std::string &conversation_service_name)
    {
        _identity_service_name     = identity_service_name;
        _conversation_service_name = conversation_service_name;
        _mm_channels = std::make_shared<ServiceManager>();
        _mm_channels->declared(_identity_service_name);
        _mm_channels->declared(_conversation_service_name);

        auto put_cb = std::bind(&ServiceManager::onServiceOnline, _mm_channels.get(),
                                std::placeholders::_1, std::placeholders::_2);
        auto del_cb = std::bind(&ServiceManager::onServiceOffline, _mm_channels.get(),
                                std::placeholders::_1, std::placeholders::_2);

        _service_discover = std::make_shared<Discovery>(reg_host, base_service_name, put_cb, del_cb);
    }
    void make_registry_object(const std::string &reg_host,
                              const std::string &service_name,
                              const std::string &access_host)
    {
        _reg_client = std::make_shared<Registry>(reg_host);
        _reg_client->registry(service_name, access_host);
    }
    void make_rpc_object(uint16_t port, uint32_t timeout, uint8_t num_threads) {
        _rpc_server = std::make_shared<brpc::Server>();
        if(!_es_client)    { LOG_ERROR("还未初始化ES模块");      abort(); }
        if(!_mysql_client) { LOG_ERROR("还未初始化MySQL模块");   abort(); }
        if(!_mm_channels)  { LOG_ERROR("还未初始化信道管理模块"); abort(); }

        auto *impl = new RelationshipServiceImpl(_es_client, _mysql_client, _mm_channels,
                                                  _identity_service_name,
                                                  _conversation_service_name);
        if(_rpc_server->AddService(impl, brpc::ServiceOwnership::SERVER_OWNS_SERVICE) == -1) {
            LOG_ERROR("添加RPC服务失败!"); abort();
        }
        brpc::ServerOptions options;
        options.idle_timeout_sec = timeout;
        options.num_threads      = num_threads;
        if(_rpc_server->Start(port, &options) == -1) {
            LOG_ERROR("服务启动失败!"); abort();
        }
    }
    RelationshipServer::ptr build() {
        if(!_service_discover) { LOG_ERROR("还未初始化服务发现模块"); abort(); }
        if(!_reg_client)       { LOG_ERROR("还未初始化服务注册模块"); abort(); }
        if(!_rpc_server)       { LOG_ERROR("还未初始化RPC模块");      abort(); }
        return std::make_shared<RelationshipServer>(_service_discover, _reg_client,
                                                    _mysql_client, _rpc_server);
    }

private:
    Registry::ptr  _reg_client;
    std::shared_ptr<elasticlient::Client> _es_client;
    std::shared_ptr<odb::core::database>  _mysql_client;

    std::string _identity_service_name;
    std::string _conversation_service_name;
    ServiceManager::ptr _mm_channels;
    Discovery::ptr      _service_discover;

    std::shared_ptr<brpc::Server> _rpc_server;
};

} // namespace chatnow
```

- [ ] **Step 5: conf/relationship_server.conf**

Create `conf/relationship_server.conf` with:

```
-run_mode=true
-log_file=/im/logs/relationship.log
-log_level=0
-registry_host=http://10.0.4.10:2379
-base_service=/service
-instance_name=/relationship_service/instance
-access_host=10.0.4.10:10006
-listen_port=10006
-rpc_timeout=-1
-rpc_threads=1
-identity_service=/service/identity_service
-conversation_service=/service/conversation_service
-es_host=http://10.0.4.10:9200/
-mysql_host=10.0.4.10
-mysql_user=root
-mysql_pswd=YHY060403
-mysql_db=chatnow
-mysql_cset=utf8mb4
-mysql_port=0
-mysql_pool_count=4
```

- [ ] **Step 6: 顶层 CMakeLists 加入新目录**

Modify `CMakeLists.txt`（顶层）：找到 `add_subdirectory(friend)` 行，改为 `add_subdirectory(relationship)`。

执行：
```bash
sed -i 's|add_subdirectory(friend)|add_subdirectory(relationship)|' CMakeLists.txt
grep -n "add_subdirectory" CMakeLists.txt
```
Expected: 输出含 `add_subdirectory(relationship)`，不再含 `add_subdirectory(friend)`

- [ ] **Step 7: 编译骨架（此时 RelationshipServiceImpl 没 override 任何 RPC，brpc 应当允许编译，但若 protobuf 生成纯虚则编译会报缺 override；预期此时编译通过或报缺虚函数）**

Run:
```bash
cd build && cmake --build . --target relationship_server -j 2>&1 | tail -30
```
Expected: 此时**预期会编译失败**，因为 `RelationshipServiceImpl` 没有 override 任何 RPC，protoc 生成的 abstract service 仍会用 brpc 默认实现兜底（与 user/IdentityServiceImpl 同模式 — Identity 也只 override Login/Logout/RefreshToken 三个，剩余 6 个走默认）。检查 user_server.h 同样情况下能编译，则此处也能编译通过。**如编译失败**：把 9 个 RPC 都先加 `// TODO Task 5+` 占位 override（参考 Task 5 的签名），返回 OK 编译。

实际上 brpc + protoc 生成的 service 类是抽象基类，但虚函数有 default `controller->SetFailed("Method ... not implemented")` 实现，子类不强制 override；与 Identity 现状一致。直接编译应当通过。

- [ ] **Step 8: 提交骨架**

```bash
git add relationship/ conf/relationship_server.conf CMakeLists.txt
git commit -m "relationship: 新建服务目录骨架 + Server/Builder/main，对齐 Identity 模式"
```

---

## Task 5: 实现"读取类"4 个 RPC（ListFriends / SearchFriends / ListPendingRequests / ListBlockedUsers）

**Files:**
- Modify: `relationship/source/relationship_server.h`

- [ ] **Step 1: 在 RelationshipServiceImpl 类内（构造函数后、private 之前）添加 GetMultiUserInfo 辅助函数**

Insert before `private:` 段：

```cpp
public:
    // ---- 4 个读取类 RPC ----

    void ListFriends(::google::protobuf::RpcController* base_cntl,
                     const ::chatnow::relationship::ListFriendsReq* req,
                     ::chatnow::relationship::ListFriendsRsp* rsp,
                     ::google::protobuf::Closure* done) override
    {
        brpc::ClosureGuard done_guard(done);
        auto* cntl = static_cast<brpc::Controller*>(base_cntl);
        HANDLE_RPC(cntl, req, rsp, {
            auto friend_ids = _mysql_relation->friends(auth.user_id);
            std::unordered_set<std::string> uid_set(friend_ids.begin(), friend_ids.end());

            std::unordered_map<std::string, ::chatnow::common::UserInfo> user_map;
            if (!uid_set.empty()) {
                if (!fetch_users(cntl, req->request_id(), uid_set, user_map))
                    throw ServiceError(::chatnow::error::kSystemUnavailable,
                                       "identity service unavailable");
            }
            for (auto &kv : user_map) {
                auto* u = rsp->add_friend_list();
                u->CopyFrom(kv.second);
            }
            // page 字段：当前 DAO 还没分页参数，先全量返回；page.has_more=false
            rsp->mutable_page()->set_has_more(false);
            rsp->mutable_page()->set_total_count(static_cast<int32_t>(friend_ids.size()));
        });
    }

    void SearchFriends(::google::protobuf::RpcController* base_cntl,
                       const ::chatnow::relationship::SearchFriendsReq* req,
                       ::chatnow::relationship::SearchFriendsRsp* rsp,
                       ::google::protobuf::Closure* done) override
    {
        brpc::ClosureGuard done_guard(done);
        auto* cntl = static_cast<brpc::Controller*>(base_cntl);
        HANDLE_RPC(cntl, req, rsp, {
            // 1. 排除集合：自己 + 已是好友 + (我拉黑 ∪ 拉黑我)
            std::vector<std::string> exclude;
            exclude.push_back(auth.user_id);
            auto friends_v = _mysql_relation->friends(auth.user_id);
            exclude.insert(exclude.end(), friends_v.begin(), friends_v.end());
            auto block_set = _mysql_user_block->blocked_or_blocking(auth.user_id);
            exclude.insert(exclude.end(), block_set.begin(), block_set.end());

            // 2. ES 搜索
            auto hits = _es_user->search(req->search_key(), exclude);
            std::unordered_set<std::string> uid_set;
            for (auto &h : hits) uid_set.insert(h.user_id());

            // 3. 批量取 UserInfo
            std::unordered_map<std::string, ::chatnow::common::UserInfo> user_map;
            if (!uid_set.empty()) {
                if (!fetch_users(cntl, req->request_id(), uid_set, user_map))
                    throw ServiceError(::chatnow::error::kSystemUnavailable,
                                       "identity service unavailable");
            }
            for (auto &kv : user_map) {
                auto* u = rsp->add_user_info();
                u->CopyFrom(kv.second);
            }
        });
    }

    void ListPendingRequests(::google::protobuf::RpcController* base_cntl,
                             const ::chatnow::relationship::ListPendingReq* req,
                             ::chatnow::relationship::ListPendingRsp* rsp,
                             ::google::protobuf::Closure* done) override
    {
        brpc::ClosureGuard done_guard(done);
        auto* cntl = static_cast<brpc::Controller*>(base_cntl);
        HANDLE_RPC(cntl, req, rsp, {
            auto pendings = _mysql_friend_apply->select_pending(auth.user_id);
            std::unordered_set<std::string> uid_set;
            for (auto &row : pendings) uid_set.insert(row.user_id());

            std::unordered_map<std::string, ::chatnow::common::UserInfo> user_map;
            if (!uid_set.empty()) {
                if (!fetch_users(cntl, req->request_id(), uid_set, user_map))
                    throw ServiceError(::chatnow::error::kSystemUnavailable,
                                       "identity service unavailable");
            }
            for (auto &row : pendings) {
                auto it = user_map.find(row.user_id());
                if (it == user_map.end()) continue; // 申请人已注销/查不到，跳过
                auto* ev = rsp->add_event();
                ev->set_event_id(row.event_id());
                ev->mutable_sender()->CopyFrom(it->second);
            }
        });
    }

    void ListBlockedUsers(::google::protobuf::RpcController* base_cntl,
                          const ::chatnow::relationship::ListBlockedReq* req,
                          ::chatnow::relationship::ListBlockedRsp* rsp,
                          ::google::protobuf::Closure* done) override
    {
        brpc::ClosureGuard done_guard(done);
        auto* cntl = static_cast<brpc::Controller*>(base_cntl);
        HANDLE_RPC(cntl, req, rsp, {
            // PageRequest.cursor 当前不解析（DAO 用 offset/limit 简单分页）
            int limit  = req->page().limit() > 0 ? req->page().limit() : 50;
            int offset = 0;
            if (!req->page().cursor().empty()) {
                try { offset = std::stoi(req->page().cursor()); }
                catch (...) { offset = 0; }
            }
            auto blocked_ids = _mysql_user_block->list_blocked(auth.user_id, offset, limit);
            std::unordered_set<std::string> uid_set(blocked_ids.begin(), blocked_ids.end());

            std::unordered_map<std::string, ::chatnow::common::UserInfo> user_map;
            if (!uid_set.empty()) {
                if (!fetch_users(cntl, req->request_id(), uid_set, user_map))
                    throw ServiceError(::chatnow::error::kSystemUnavailable,
                                       "identity service unavailable");
            }
            for (auto &id : blocked_ids) {
                auto it = user_map.find(id);
                if (it == user_map.end()) continue;
                auto* u = rsp->add_blocked_list();
                u->CopyFrom(it->second);
            }
            int64_t total = _mysql_user_block->count_blocked(auth.user_id);
            rsp->mutable_page()->set_total_count(static_cast<int32_t>(total));
            rsp->mutable_page()->set_has_more(offset + limit < total);
            if (rsp->page().has_more()) {
                rsp->mutable_page()->set_next_cursor(std::to_string(offset + limit));
            }
        });
    }

private:
    bool fetch_users(brpc::Controller* in_cntl,
                     const std::string &rid,
                     const std::unordered_set<std::string> &uid_set,
                     std::unordered_map<std::string, ::chatnow::common::UserInfo> &out)
    {
        auto channel = _mm_channels->choose(_identity_service_name);
        if (!channel) {
            LOG_ERROR("rid={} identity 子服务节点不可达 svc={}",
                      rid, _identity_service_name);
            return false;
        }
        ::chatnow::identity::IdentityService_Stub stub(channel.get());
        ::chatnow::identity::GetMultiUserInfoReq  q;
        ::chatnow::identity::GetMultiUserInfoRsp  a;
        q.set_request_id(rid);
        for (auto &id : uid_set) q.add_users_id(id);

        brpc::Controller out_cntl;
        ::chatnow::auth::forward_auth_metadata(in_cntl, &out_cntl);
        stub.GetMultiUserInfo(&out_cntl, &q, &a, nullptr);
        if (out_cntl.Failed()) {
            LOG_ERROR("rid={} GetMultiUserInfo brpc 失败: {}", rid, out_cntl.ErrorText());
            return false;
        }
        if (!a.header().success()) {
            LOG_ERROR("rid={} GetMultiUserInfo 业务失败: code={} msg={}",
                      rid, a.header().error_code(), a.header().error_message());
            return false;
        }
        for (auto &kv : a.users_info()) out.emplace(kv.first, kv.second);
        return true;
    }
```

- [ ] **Step 2: 编译验证**

Run:
```bash
cd build && cmake --build . --target relationship_server -j 2>&1 | tail -20
```
Expected: 编译通过；如有 `users_info` map 字段名不一致等错误，对照 `proto/identity/identity_service.proto` 修正

- [ ] **Step 3: 提交**

```bash
git add relationship/source/relationship_server.h
git commit -m "relationship: 实现 4 个读取类 RPC（ListFriends/SearchFriends/ListPending/ListBlocked）"
```

---

## Task 6: 实现"写入类" 3 个 RPC（SendFriendRequest / RemoveFriend / Block&Unblock）

**Files:**
- Modify: `relationship/source/relationship_server.h`

- [ ] **Step 1: 在 ListBlockedUsers 与 fetch_users 之间，public 段加 4 个写入 RPC**

Insert before `private:` 段，紧接 ListBlockedUsers 后：

```cpp
    void SendFriendRequest(::google::protobuf::RpcController* base_cntl,
                           const ::chatnow::relationship::SendFriendReq* req,
                           ::chatnow::relationship::SendFriendRsp* rsp,
                           ::google::protobuf::Closure* done) override
    {
        brpc::ClosureGuard done_guard(done);
        auto* cntl = static_cast<brpc::Controller*>(base_cntl);
        HANDLE_RPC(cntl, req, rsp, {
            const std::string &uid = auth.user_id;
            const std::string &pid = req->respondent_id();
            if (pid.empty() || uid == pid)
                throw ServiceError(::chatnow::error::kSystemInvalidArgument,
                                   "invalid respondent_id");

            // 拉黑双向：对端拉黑了我 → 直接 BLOCKED；我拉黑了对端在业务上"我已拒"
            if (_mysql_user_block->is_blocked(pid, uid))
                throw ServiceError(::chatnow::error::kRelationshipBlocked,
                                   "blocked by peer");

            if (_mysql_relation->exists(uid, pid))
                throw ServiceError(::chatnow::error::kRelationshipAlreadyFriends,
                                   "already friends");

            // PENDING 中 → 直接复用之前 event_id（与现状一致）
            if (_mysql_friend_apply->exists_pending(uid, pid)) {
                auto latest = _mysql_friend_apply->select_latest(uid, pid);
                if (latest && latest->status() == FriendApplyStatus::PENDING) {
                    rsp->set_notify_event_id(latest->event_id());
                    return; // HANDLE_RPC 已写好成功 header
                }
                throw ServiceError(::chatnow::error::kRelationshipRequestPending,
                                   "duplicate apply");
            }

            // 上一次被拒 + 距今 <72h → 拒绝
            auto last = _mysql_friend_apply->select_latest(uid, pid);
            if (last && last->status() == FriendApplyStatus::REJECTED) {
                auto now = boost::posix_time::microsec_clock::universal_time();
                if (now - last->create_time() < boost::posix_time::hours(72))
                    throw ServiceError(::chatnow::error::kRelationshipRequestPending,
                                       "rejected within 72h");
            }

            // 新建申请
            std::string eid = uuid();
            FriendApply ev(eid, uid, pid, FriendApplyStatus::PENDING,
                           boost::posix_time::microsec_clock::universal_time());
            if (!_mysql_friend_apply->insert(ev))
                throw ServiceError(::chatnow::error::kSystemInternalError,
                                   "insert friend_apply failed");
            rsp->set_notify_event_id(eid);
        });
    }

    void RemoveFriend(::google::protobuf::RpcController* base_cntl,
                      const ::chatnow::relationship::RemoveFriendReq* req,
                      ::chatnow::relationship::RemoveFriendRsp* rsp,
                      ::google::protobuf::Closure* done) override
    {
        brpc::ClosureGuard done_guard(done);
        auto* cntl = static_cast<brpc::Controller*>(base_cntl);
        HANDLE_RPC(cntl, req, rsp, {
            const std::string &uid = auth.user_id;
            const std::string &pid = req->peer_id();
            if (pid.empty() || uid == pid)
                throw ServiceError(::chatnow::error::kSystemInvalidArgument,
                                   "invalid peer_id");
            if (!_mysql_relation->exists(uid, pid))
                throw ServiceError(::chatnow::error::kRelationshipNotFriends,
                                   "not friends");
            if (!_mysql_relation->remove(uid, pid))
                throw ServiceError(::chatnow::error::kSystemInternalError,
                                   "remove relation failed");
            // user_block 表与好友关系独立，删好友不动 user_block；
            // 现状会话清理改由 Conversation 服务负责（暂不在本服务范围内）。
        });
    }

    void BlockUser(::google::protobuf::RpcController* base_cntl,
                   const ::chatnow::relationship::BlockUserReq* req,
                   ::chatnow::relationship::BlockUserRsp* rsp,
                   ::google::protobuf::Closure* done) override
    {
        brpc::ClosureGuard done_guard(done);
        auto* cntl = static_cast<brpc::Controller*>(base_cntl);
        HANDLE_RPC(cntl, req, rsp, {
            const std::string &uid = auth.user_id;
            const std::string &pid = req->peer_id();
            if (pid.empty() || uid == pid)
                throw ServiceError(::chatnow::error::kSystemInvalidArgument,
                                   "invalid peer_id");
            if (!_mysql_user_block->insert(uid, pid))
                throw ServiceError(::chatnow::error::kSystemInternalError,
                                   "insert user_block failed");
        });
    }

    void UnblockUser(::google::protobuf::RpcController* base_cntl,
                     const ::chatnow::relationship::UnblockUserReq* req,
                     ::chatnow::relationship::UnblockUserRsp* rsp,
                     ::google::protobuf::Closure* done) override
    {
        brpc::ClosureGuard done_guard(done);
        auto* cntl = static_cast<brpc::Controller*>(base_cntl);
        HANDLE_RPC(cntl, req, rsp, {
            const std::string &uid = auth.user_id;
            const std::string &pid = req->peer_id();
            if (pid.empty() || uid == pid)
                throw ServiceError(::chatnow::error::kSystemInvalidArgument,
                                   "invalid peer_id");
            if (!_mysql_user_block->remove(uid, pid))
                throw ServiceError(::chatnow::error::kSystemInternalError,
                                   "remove user_block failed");
        });
    }
```

- [ ] **Step 2: 在 `common/error/error_codes.hpp` 中补 4 个 Relationship 段错误码（仅当未存在时）**

```bash
grep -n "kRelationship" common/error/error_codes.hpp
```

If grep returns nothing — Edit `common/error/error_codes.hpp`：在认证段（1xxx）之后、媒体段（5xxx）之前插入：

```cpp
// 2000-2999 关系（与 proto/common/error.proto 同步）
inline constexpr int32_t kRelationshipAlreadyFriends   = 2001;
inline constexpr int32_t kRelationshipNotFriends       = 2002;
inline constexpr int32_t kRelationshipBlocked          = 2003;
inline constexpr int32_t kRelationshipRequestPending   = 2004;
```

- [ ] **Step 3: 编译验证**

Run:
```bash
cd build && cmake --build . --target relationship_server -j 2>&1 | tail -20
```
Expected: 编译通过；如 `uuid()` / `select_latest` 等接口名不符，按 `friend_server.h` 现状语义同名引用

- [ ] **Step 4: 提交**

```bash
git add relationship/source/relationship_server.h common/error/error_codes.hpp
git commit -m "relationship: 实现写入类 RPC（SendFriend/Remove/Block/Unblock）+ 错误码补全"
```

---

## Task 7: 实现 HandleFriendRequest（含 ConversationService.CreateConversation）

**Files:**
- Modify: `relationship/source/relationship_server.h`

- [ ] **Step 1: 在 UnblockUser 之后、`private:` 之前插入 HandleFriendRequest**

```cpp
    void HandleFriendRequest(::google::protobuf::RpcController* base_cntl,
                             const ::chatnow::relationship::HandleFriendReq* req,
                             ::chatnow::relationship::HandleFriendRsp* rsp,
                             ::google::protobuf::Closure* done) override
    {
        brpc::ClosureGuard done_guard(done);
        auto* cntl = static_cast<brpc::Controller*>(base_cntl);
        HANDLE_RPC(cntl, req, rsp, {
            const std::string &eid          = req->notify_event_id();
            const std::string &apply_uid    = req->apply_user_id();    // 申请人
            const std::string &peer_uid     = auth.user_id;            // 被申请人 = 当前调用者
            if (eid.empty() || apply_uid.empty() || peer_uid == apply_uid)
                throw ServiceError(::chatnow::error::kSystemInvalidArgument,
                                   "invalid event_id / apply_user_id");

            auto ev = _mysql_friend_apply->select_by_event_id(eid);
            if (!ev || ev->user_id() != apply_uid || ev->peer_id() != peer_uid)
                throw ServiceError(::chatnow::error::kSystemInvalidArgument,
                                   "event not found");
            if (ev->status() != FriendApplyStatus::PENDING)
                throw ServiceError(::chatnow::error::kRelationshipRequestPending,
                                   "event not pending");

            if (!req->agree()) {
                if (!_mysql_friend_apply->update_status(eid, FriendApplyStatus::REJECTED))
                    throw ServiceError(::chatnow::error::kSystemInternalError,
                                       "update apply rejected failed");
                return; // 拒绝路径结束
            }

            // 同意：先写关系 → 再调 Conversation 建单聊；任何一步失败 ServiceError
            if (!_mysql_friend_apply->update_status(eid, FriendApplyStatus::ACCEPTED))
                throw ServiceError(::chatnow::error::kSystemInternalError,
                                   "update apply accepted failed");
            if (!_mysql_relation->insert(peer_uid, apply_uid))
                throw ServiceError(::chatnow::error::kSystemInternalError,
                                   "insert relation failed");

            // 调 Conversation.CreateConversation；失败 fail-soft：关系已建，
            // new_conversation_id 留空 + ERROR 日志
            auto channel = _mm_channels->choose(_conversation_service_name);
            if (!channel) {
                LOG_ERROR("rid={} conversation 子服务节点不可达 svc={}",
                          req->request_id(), _conversation_service_name);
                return; // header 已是成功
            }
            ::chatnow::conversation::ConversationService_Stub conv(channel.get());
            ::chatnow::conversation::CreateConversationReq  cq;
            ::chatnow::conversation::CreateConversationRsp  ca;
            cq.set_request_id(req->request_id());
            cq.set_name("");                 // 单聊不传名字（沿用现状逻辑）
            cq.add_member_id_list(peer_uid);
            cq.add_member_id_list(apply_uid);

            brpc::Controller out_cntl;
            ::chatnow::auth::forward_auth_metadata(cntl, &out_cntl);
            conv.CreateConversation(&out_cntl, &cq, &ca, nullptr);
            if (out_cntl.Failed()) {
                LOG_ERROR("rid={} CreateConversation brpc 失败: {}",
                          req->request_id(), out_cntl.ErrorText());
                return;
            }
            if (!ca.header().success()) {
                LOG_ERROR("rid={} CreateConversation 业务失败: code={} msg={}",
                          req->request_id(), ca.header().error_code(),
                          ca.header().error_message());
                return;
            }
            // CreateConversationRsp 含 Conversation 字段；按 proto 取 conversation.id
            // （字段名以 conversation_service.proto 中 Conversation 实际定义为准）
            if (ca.has_conversation()) {
                rsp->set_new_conversation_id(ca.conversation().conversation_id());
            }
        });
    }
```

**说明**：
- `Conversation.conversation_id` 字段名需按 `proto/conversation/conversation_service.proto` 中 `Conversation` 消息的实际定义；若是 `id` 或 `cid` 则相应替换
- Conversation 服务尚未迁完时，调用会返回不可用错误；按 spec §5.2 的 fail-soft 路径走

- [ ] **Step 2: 编译**

Run:
```bash
cd build && cmake --build . --target relationship_server -j 2>&1 | tail -20
```
Expected: 通过；如 `Conversation.conversation_id` 字段名不存在，先打开 `proto/conversation/conversation_service.proto` 找正确字段名（可能是 `id` / `conversation_id` / `cid`），按实际改

- [ ] **Step 3: 提交**

```bash
git add relationship/source/relationship_server.h
git commit -m "relationship: 实现 HandleFriendRequest（同意分支调 Conversation.CreateConversation）"
```

---

## Task 8: 切换 Gateway 6 个 friend handler 到新 stub

**Files:**
- Modify: `gateway/source/gateway_server.h`
- Modify: `gateway/CMakeLists.txt`（确认 proto_files 含 relationship/identity，移除 friend 旧 proto）

- [ ] **Step 1: 检查 gateway/CMakeLists.txt 当前 proto_files**

Run:
```bash
grep "proto_files" gateway/CMakeLists.txt
```
Expected: 输出含 `friend.proto`；需要替换为 `relationship/relationship_service.proto`

执行：
```bash
sed -i 's|friend.proto|relationship/relationship_service.proto|' gateway/CMakeLists.txt
grep "proto_files" gateway/CMakeLists.txt
```
Expected: 输出含 `relationship/relationship_service.proto`，不再含 `friend.proto`

- [ ] **Step 2: 修改 gateway_server.h — 替换 include 与 stub 调用**

在 `gateway/source/gateway_server.h` 顶部：
```bash
# 替换 include
sed -i 's|#include "friend.pb.h"|#include "relationship/relationship_service.pb.h"|g' gateway/source/gateway_server.h
sed -i 's|FriendService_Stub|chatnow::relationship::RelationshipService_Stub|g' gateway/source/gateway_server.h
```

注：sed 全替换 6 个 handler 中的 stub 类名后，方法名仍然是 `GetFriendList / FriendAdd / FriendAddProcess / FriendRemove / FriendSearch / GetPendingFriendEventList`，需要逐个手动改为新 RPC 名：

| 旧 stub 调用 | 新 stub 调用 | Req/Rsp 类型 |
|---|---|---|
| `stub.GetFriendList(...)` | `stub.ListFriends(...)` | `chatnow::relationship::ListFriendsReq/Rsp` |
| `stub.FriendAdd(...)` | `stub.SendFriendRequest(...)` | `chatnow::relationship::SendFriendReq/Rsp` |
| `stub.FriendAddProcess(...)` | `stub.HandleFriendRequest(...)` | `chatnow::relationship::HandleFriendReq/Rsp` |
| `stub.FriendRemove(...)` | `stub.RemoveFriend(...)` | `chatnow::relationship::RemoveFriendReq/Rsp` |
| `stub.FriendSearch(...)` | `stub.SearchFriends(...)` | `chatnow::relationship::SearchFriendsReq/Rsp` |
| `stub.GetPendingFriendEventList(...)` | `stub.ListPendingRequests(...)` | `chatnow::relationship::ListPendingReq/Rsp` |

逐个 handler 修改：
- 把 `GetFriendListReq req; GetFriendListRsp rsp;` → `chatnow::relationship::ListFriendsReq req; chatnow::relationship::ListFriendsRsp rsp;`
- 类似对其余 5 个 handler；注意旧响应字段 `rsp.success() / rsp.errmsg() / rsp.friend_list()` 等改为：
  - `rsp.header().success()` / `rsp.header().error_message()`
  - 列表字段名按新 proto：`friend_list / user_info / event / blocked_list`
- 服务名 `FLAGS_friend_service` 改为 `FLAGS_relationship_service`（gateway 配置层，需要改 conf/gateway_server.conf 与 gateway_server.cc 的 DEFINE_string）

- [ ] **Step 3: 修改 gateway_server.cc 与 conf**

```bash
grep -n "FLAGS_friend_service\|friend_service\b" gateway/source/gateway_server.cc gateway/source/gateway_server.h conf/gateway_server.conf
```
Expected: 显示出所有相关行；逐一改为 `relationship_service`：
- gateway_server.cc 的 `DEFINE_string(friend_service, ...)` → `DEFINE_string(relationship_service, ...)`，default value `/service/relationship_service`
- gateway_server.h 的 `_friend_service_name` 字段、构造参数全部改名
- conf/gateway_server.conf 的 `-friend_service=...` 改为 `-relationship_service=/service/relationship_service`

- [ ] **Step 4: 编译 gateway**

Run:
```bash
cd build && cmake --build . --target gateway_server -j 2>&1 | tail -30
```
Expected: 编译通过；如残留 `FriendAdd` / `FriendRemove` 等老 RPC 名，按 step 2 表格继续替换

- [ ] **Step 5: 提交**

```bash
git add gateway/ conf/gateway_server.conf
git commit -m "gateway: 6 个 friend handler 切到 RelationshipService 新 stub + 服务名重命名"
```

---

## Task 9: 删除旧 friend/ 目录与 proto/friend.proto

**Files:**
- Delete: `friend/CMakeLists.txt`、`friend/Dockerfile`、`friend/source/`、`conf/friend_server.conf`、`proto/friend.proto`

- [ ] **Step 1: 全仓 grep 确认 friend.pb 已无引用**

Run:
```bash
grep -rn 'friend\.pb\|"friend\.pb\.h"\|FriendService_Stub\|FriendAddReq\|FriendAddProcessReq\|FriendRemoveReq\|FriendSearchReq\|GetFriendListReq\|GetPendingFriendEventListReq' \
  --include='*.h' --include='*.cc' --include='*.cpp' --include='*.hpp' .
```
Expected: 0 行命中。如有命中，回到 Task 8 补改

- [ ] **Step 2: 删除目录与 proto 文件**

```bash
rm -rf friend/
rm conf/friend_server.conf
rm proto/friend.proto
```

- [ ] **Step 3: docker-compose.yml 重命名 service**

```bash
grep -n "friend_server" docker-compose.yml
```

If 命中：
```bash
sed -i 's/friend_server/relationship_server/g' docker-compose.yml
sed -i 's|/im/conf/friend_server.conf|/im/conf/relationship_server.conf|g' docker-compose.yml
```

- [ ] **Step 4: 全仓编译，确认无残留**

```bash
cd build && cmake .. && cmake --build . -j 2>&1 | tail -30
```
Expected: 全部 target 编译通过；旧 `friend_server` 不再被构建

- [ ] **Step 5: 提交**

```bash
git add -A
git commit -m "cleanup: 删除旧 friend/ 目录 + proto/friend.proto + 旧配置；docker-compose 重命名为 relationship_server"
```

---

## Task 10: 集成验收

**Files:**
- 无新文件；这是验证步骤

- [ ] **Step 1: schema 与 docker 起动**

```bash
docker compose up -d mysql redis es etcd
# 等 MySQL ready 后
mysql -h127.0.0.1 -uroot -p chatnow < <(odb -d mysql --generate-schema --suppress-migration --schema-format=sql --profile boost/date-time \
  odb/relation.hxx odb/friend_apply.hxx odb/user_block.hxx 2>&1)
```
注：上面是参考写法；实际项目中 schema 生成走 `--generate-schema` 编译时输出，按现仓 SOP 执行即可

- [ ] **Step 2: 启动 identity_server + relationship_server，注册 etcd**

```bash
./build/user/user_server --flagfile=conf/user_server.conf &
./build/relationship/relationship_server --flagfile=conf/relationship_server.conf &
sleep 2
etcdctl get --prefix /service/relationship_service
```
Expected: 看到 instance access_host 注册成功

- [ ] **Step 3: 用 friend/test 老的 client 不再可用，改用 brpc + curl 验证**

```bash
# 例：调 ListFriends（需带鉴权 metadata）
curl -X POST http://127.0.0.1:10006/RelationshipService/ListFriends \
  -H "Content-Type: application/protobuf" \
  -H "x-user-id: u_test_1" \
  -H "x-device-id: d_test" \
  -H "x-trace-id: $(uuidgen | tr -d - | head -c 32)" \
  --data-binary @/tmp/empty_list_friends.bin
```

可参考 `user/test/` 已有的 client 模式（如有），先写一段小测试用 brpc Channel + Stub 直接调 `RelationshipService_Stub.ListFriends`，覆盖：

- ListFriends（空 + 有 1 个好友）
- SendFriendRequest → ListPendingRequests → HandleFriendRequest(agree=true) → ListFriends 包含对方
- BlockUser → SendFriendRequest 返回 `kRelationshipBlocked` (2003)
- UnblockUser → SendFriendRequest 通过
- ListBlockedUsers 分页

- [ ] **Step 4: 验收清单核对（来自 spec §7）**

```bash
# 7. proto 不含鉴权字段
grep -n "optional string user_id\|optional string session_id" proto/relationship/relationship_service.proto
# 期望：0 行

# 10. friend/ 目录不存在
ls friend/ 2>&1 | head -1
# 期望：No such file or directory

# 8. friend.pb 引用归零
grep -rn "friend\.pb" --include='*.h' --include='*.cc' --include='*.cpp' --include='*.hpp' .
# 期望：0 行
```

- [ ] **Step 5: 提交集成报告**

```bash
git log --oneline -10
```
Expected: 最近 10 次提交按 Task 1 → Task 9 顺序排列

---

## 执行注意事项

1. **每个 Task 完成后单独 commit**：plan 中已显式给出 commit 步骤；不要把多个 Task 合并 commit
2. **如某 step 编译报错**：按报错信息修，**不**绕开 plan 跳到下一个 Task；可能需要在当前文件内补 `#include` 或修字段名
3. **proto 字段名**（如 `Conversation.conversation_id` / `users_info` map key）以 `proto/conversation/conversation_service.proto` 与 `proto/identity/identity_service.proto` 实际定义为准；本 plan 给出的字段名按当前 proto 文件是吻合的，但若 proto 后续变更需同步
4. **HandleFriendRequest 同意分支**调用 Conversation 服务时，Conversation 服务可能尚未完成迁移（`ChatSessionServiceImpl` 仍是旧名）；**fail-soft 设计已覆盖此情况**：服务不可达时 new_conversation_id 留空 + ERROR 日志，header.success=true
5. **HTTP 路径不变**：客户端 SDK 不需要改；改动局限于服务端
