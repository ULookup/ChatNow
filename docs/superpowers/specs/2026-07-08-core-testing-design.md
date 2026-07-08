# ChatNow 核心功能单元测试与集成测试设计

> **状态**: 设计完成，待评审
> **日期**: 2026-07-08
> **范围**: 核心功能测试架构（单元测试 + DAO 集成测试 + 端到端测试），测试框架 gtest + gmock
> **基线**: 现有 `common/test/` 14 个工具类单元测试 + `file/test/` 2 个 MinIO 集成测试
> **目标**: 建立 C++ 微服务测试金字塔，覆盖核心消息链路，CI 自动化驱动

---

## 0. 设计原则

1. **测试金字塔** - 单元测试多（快、无依赖）、集成测试中（真 DB/MQ）、E2E 少（全链路）
2. **接口抽取可测试性** - ServiceImpl 持有抽象接口而非具体类型，用 gmock 隔离
3. **文件拆分清晰** - 接口（`i_*.hpp`）与实现（`<concrete>.hpp`）分文件，接口头无外部依赖
4. **CI 与本地一致** - 同一套 `ctest -L` 命令，仅靠环境变量区分层级
5. **YAGNI** - 不引入 testcontainers-cpp（不成熟），用 docker-compose；不引入额外 mock 框架，用 gtest 自带 gmock

明确**不做**的事：
- ❌ 不 mock DAO 层（SQL/ORM 行为本身是被测对象，用真实 DB）
- ❌ 不在 macOS runner 上跑 CI（目标环境是 Linux）
- ❌ 不测 brpc controller 本身（传 nullptr 或 fake）
- ❌ Phase 1 不覆盖 gateway/friend/chatsession/push（放后续 phase）

---

## 1. 整体架构

### 1.1 测试金字塔

```
                    ┌──────────┐
                    │   E2E    │  几个关键链路，全套 docker-compose
                    │  (少)    │  PR to main + nightly
                   /└──────────┘\
                  /              \
          ┌──────────┐      ┌──────────┐
          │  DAO 集成 │      │  单元测试 │  gmock 隔离，无外部依赖
          │  (中)    │      │  (多)    │  每次 push
          └──────────┘      └──────────┘
         真 MySQL/Redis/      ServiceImpl 业务逻辑
         ES/MQ/MinIO          DAO 接口被 mock
         docker-compose
         PR 触发
```

### 1.2 目录结构（最终形态）

```
ChatNow/
├── common/test/                          # 已有，保持（工具类单元测试）
├── file/test/
│   ├── unit/                             # 新增：UploadHandler/MultipartHandler 业务逻辑
│   └── integration/                      # 迁移：test_s3_integration.cc 等
├── transmite/test/
│   ├── unit/                             # 新增：GetTransmitTarget 逻辑 + 校验
│   └── integration/                      # 新增：真 brpc + 真 MQ
├── message/test/
│   ├── unit/                             # 新增：GetHistoryMsg/GetOfflineMsg 逻辑
│   └── integration/                      # 新增：真 MySQL + 真 ES 双写
├── gateway/test/
│   ├── unit/
│   └── integration/
├── tests/
│   ├── e2e/                              # 跨服务链路测试
│   │   ├── test_message_pipeline.cc      # 发消息全链路
│   │   ├── test_message_offline_sync.cc  # 离线同步全链路
│   │   ├── test_media_upload.cc          # 三步上传全链路
│   │   ├── helpers/
│   │   │   ├── http_client.hpp
│   │   │   ├── ws_client.hpp
│   │   │   └── test_users.hpp
│   │   └── CMakeLists.txt
│   ├── mocks/                            # 共享 gmock 类（跨服务复用）
│   │   ├── mock_publisher.hpp
│   │   ├── mock_channel_manager.hpp
│   │   ├── mock_snowflake.hpp
│   │   └── ...
│   └── fixtures/                         # 共享 fixture
│       ├── db_fixture.hpp
│       ├── mq_fixture.hpp
│       ├── fake_closure.hpp
│       └── proto_helpers.hpp
├── docker/
│   └── docker-compose.test.yml           # 仅基础设施（不含业务服务），给集成测试用
├── scripts/
│   ├── wait_for_infra.sh
│   └── wait_for_services.sh
└── .github/workflows/ci.yml              # 单文件，三 job 串联
```

### 1.3 CI workflow 形态

单个 `ci.yml`，三个 job 用 `needs` 串联：

```
push/PR ──> unit (无依赖, ~1min)
               │ 失败则短路
               ▼
            integration (docker-compose.test.yml, ~5min)
               │ 失败则短路
               ▼ (仅 PR to main / nightly)
            e2e (全套 docker-compose.yml + 业务服务, ~10min)
```

- `unit`：每次 push + PR，始终跑
- `integration`：每次 PR
- `e2e`：PR to main + nightly schedule

### 1.4 CTest 组织

每个 `test/` 目录的 `CMakeLists.txt` 用 `gtest_discover_tests` 注册测试，并通过 `LABELS` 标记层级：

```cmake
gtest_discover_tests(target_unit PROPERTIES LABELS "unit")
gtest_discover_tests(target_integration PROPERTIES LABELS "integration")
```

本地和 CI 统一用 `ctest -L unit` / `ctest -L integration` / `ctest -L e2e` 切换层级。

---

## 2. 接口抽取设计

### 2.1 抽取模式

每个外部依赖抽成纯虚接口，生产代码持有接口指针，测试用 gmock 实现：

```cpp
// mq/i_publisher.hpp（接口头文件，轻量无外部依赖）
#pragma once
#include <string>
#include "mq/trace_headers.hpp"

namespace chatnow {

class IPublisher {
public:
    virtual ~IPublisher() = default;
    virtual bool publish(const std::string& exchange, const std::string& routing_key,
                         const std::string& body, const MQHeaders& headers) = 0;
};

} // namespace chatnow

// mq/rabbitmq.hpp（具体实现，依赖 AMQP-CPP）
#pragma once
#include "mq/i_publisher.hpp"
// ... AMQP-CPP headers ...
namespace chatnow {
class Publisher : public IPublisher { /* 现有实现不变 */ };
}

// tests/mocks/mock_publisher.hpp
#pragma once
#include <gmock/gmock.h>
#include "mq/i_publisher.hpp"
namespace chatnow {
class MockPublisher : public IPublisher {
public:
    MOCK_METHOD4(publish, bool(const std::string&, const std::string&,
                               const std::string&, const MQHeaders&));
};
}
```

### 2.2 文件拆分约定

接口与实现分文件，接口头文件保持轻量（无外部依赖），ServiceImpl 只 include 接口头，不拖入具体实现依赖：

```
mq/
├── i_publisher.hpp          # IPublisher 纯接口
├── rabbitmq.hpp             # Publisher 具体实现
├── i_subscriber.hpp
└── trace_headers.hpp

dao/
├── i_message.hpp            # IMessageTable 接口
├── mysql_message.hpp        # MessageTable 实现
├── i_user_timeline.hpp
├── mysql_user_timeline.hpp
├── i_chat_session_member.hpp
├── mysql_chat_session_member.hpp
└── ...

common/clients/              # 新增目录
├── i_user_client.hpp        # IUserClient 接口
├── user_client.hpp          # UserClient 实现
├── i_chatsession_client.hpp
├── chatsession_client.hpp
├── i_file_client.hpp
├── file_client.hpp
└── ...

infra/
├── i_snowflake.hpp
├── snowflake.hpp
├── i_etcd.hpp
├── etcd.hpp
└── ...

tests/mocks/
├── mock_publisher.hpp       # 仅 include i_publisher.hpp
├── mock_message_table.hpp
├── mock_user_client.hpp
├── mock_snowflake.hpp
└── ...
```

### 2.3 Phase 1 需要抽取的接口清单

**transmite 服务（TransmiteServiceImpl）：**

| 现有具体类型 | 接口 | 用途 |
|---|---|---|
| `ServiceManager` | `IUserClient` / `IChatSessionClient` | RPC 调用（按业务方法分包，非按 channel） |
| `Publisher` | `IPublisher` | MQ 投递 |
| `SnowflakeId` | `IIdGenerator` | message_id 生成 |
| `SeqGen` | `ISeqGen` | 会话内序号 |
| `Members` | `IMembersCache` | 成员列表缓存 |
| `RateLimiter` | `IRateLimiter` | 限流 |

**message 服务（MessageServiceImpl）：**

| 现有具体类型 | 接口 | 用途 |
|---|---|---|
| `MessageTable` | `IMessageTable` | message 表 CRUD |
| `UserTimeLineTable` | `IUserTimeLineTable` | timeline 写扩散 |
| `ChatSessionMemberTable` | `IChatSessionMemberTable` | 成员查询 |
| `ESMessage` | `IESMessage` | ES 索引 |
| `Publisher` | `IPublisher` | push outbox / es outbox |
| `ServiceManager` | `IFileClient` / `IUserClient` | RPC 回查 |

### 2.4 RPC 调用的 mock 策略

不 mock `ServiceManager` 本身（太底层），而是为每个下游服务抽**业务客户端接口**：

```cpp
// common/clients/i_user_client.hpp
class IUserClient {
public:
    virtual ~IUserClient() = default;
    virtual bool get_user_info(const std::string& uid, UserInfo* out) = 0;
};

// common/clients/user_client.hpp
class UserClient : public IUserClient {
    // 内部用 ServiceManager::choose("UserService") + stub 调用
};
```

测试只需 `EXPECT_CALL(mock_user, get_user_info("u1", _))` 而非构造假 channel + 假 stub。

### 2.5 ServiceImpl 构造函数变化

```cpp
// 改造前
TransmiteServiceImpl(const std::string& svc_name, ServiceManager::ptr channels,
                     Publisher::ptr pub, SnowflakeId id_gen, ...);

// 改造后
TransmiteServiceImpl(std::shared_ptr<IUserClient> user,
                     std::shared_ptr<IChatSessionClient> chatsession,
                     std::shared_ptr<IPublisher> pub,
                     std::shared_ptr<IIdGenerator> id_gen, ...);
```

Builder 负责组装具体实现，测试直接注入 mock。

---

## 3. 单元测试设计

### 3.1 测试目标

针对每个 `ServiceImpl` 的 RPC handler，覆盖三类场景：

1. **正常路径** - 合法输入 + 依赖正常返回 -> 期望输出
2. **输入校验** - 非法请求（缺字段、类型不匹配、超限）-> 返回错误码
3. **依赖失败** - 下游 RPC 超时 / MQ 投递失败 / DB 冲突 -> 错误传播 + 幂等性

### 3.2 Phase 1 单元测试文件清单

```
transmite/test/unit/
├── CMakeLists.txt
├── test_transmite_new_message.cc      # GetTransmitTarget 主流程
├── test_transmite_validation.cc       # 消息类型校验（image/file_id 必填等）
├── test_transmite_large_group.cc      # 大群读扩散分支（>=200 成员）
└── test_transmite_idempotency.cc      # client_msg_id 去重

message/test/unit/
├── CMakeLists.txt
├── test_message_history.cc            # GetHistoryMsg：timeline 查询 + 批量回查
├── test_message_offline.cc            # GetOfflineMsg：游标增量拉取
├── test_message_search.cc             # MsgSearch：ES 关键字检索
├── test_message_unread.cc             # GetUnreadCount：last_read_msg 计算
├── test_message_db_consumer.cc        # MQ DB consumer 回调（写 message + timeline）
└── test_message_es_consumer.cc        # MQ ES consumer 回调（仅文本写 ES）
```

### 3.3 测试结构模式

```cpp
// test_transmite_new_message.cc
class TransmiteNewMessageTest : public ::testing::Test {
protected:
    void SetUp() override {
        _user = std::make_shared<MockUserClient>();
        _chatsession = std::make_shared<MockChatSessionClient>();
        _publisher = std::make_shared<MockPublisher>();
        _id_gen = std::make_shared<MockIdGenerator>();
        _seq_gen = std::make_shared<MockSeqGen>();
        _members = std::make_shared<MockMembersCache>();
        _rate_limiter = std::make_shared<MockRateLimiter>();

        _svc = std::make_unique<TransmiteServiceImpl>(
            _user, _chatsession, _publisher, _id_gen, _seq_gen, _members, _rate_limiter);
    }

    std::shared_ptr<MockUserClient>         _user;
    std::shared_ptr<MockChatSessionClient>  _chatsession;
    std::shared_ptr<MockPublisher>          _publisher;
    std::shared_ptr<MockIdGenerator>        _id_gen;
    std::shared_ptr<MockSeqGen>             _seq_gen;
    std::shared_ptr<MockMembersCache>       _members;
    std::shared_ptr<MockRateLimiter>        _rate_limiter;
    std::unique_ptr<TransmiteServiceImpl>   _svc;
};

TEST_F(TransmiteNewMessageTest, Success_GroupMessage) {
    NewMessageReq req;
    req.set_user_id("u1");
    req.set_chat_session_id("s1");
    req.mutable_message()->set_message_type(MessageType::TEXT);
    req.mutable_message()->mutable_text_message()->set_content("hello");

    EXPECT_CALL(*_user, get_user_info("u1", _))
        .WillOnce(DoAll(SetArgPointee<1>(make_user("u1", "alice")), Return(true)));
    EXPECT_CALL(*_chatsession, get_member_id_list("s1", _))
        .WillOnce(DoAll(SetArgPointee<1>(std::vector<std::string>{"u1","u2","u3"}), Return(true)));
    EXPECT_CALL(*_id_gen, next()).WillOnce(Return(12345LL));
    EXPECT_CALL(*_rate_limiter, allow("u1")).WillOnce(Return(true));
    EXPECT_CALL(*_publisher, publish(_, _, _, _)).WillOnce(Return(true));

    GetTransmitTargetRsp rsp;
    auto done = std::make_unique<FakeClosure>();
    _svc->GetTransmitTarget(nullptr, &req, &rsp, done.get());

    EXPECT_TRUE(rsp.success());
    EXPECT_EQ(rsp.message().message_id(), 12345LL);
    EXPECT_EQ(rsp.target_id_list_size(), 3);
}

TEST_F(TransmiteNewMessageTest, Rejects_ImageWithoutFileId) {
    NewMessageReq req;
    req.set_user_id("u1");
    req.mutable_message()->set_message_type(MessageType::IMAGE);
    // image_message.file_id() 未设置

    GetTransmitTargetRsp rsp;
    _svc->GetTransmitTarget(nullptr, &req, &rsp, nullptr);

    EXPECT_FALSE(rsp.success());
    EXPECT_EQ(rsp.errmsg(), "image message requires file_id");
    EXPECT_CALL(*_publisher, publish(_, _, _, _)).Times(0);
}
```

### 3.4 关键约定

1. **每个 RPC handler 一个 test 文件** - 文件按 handler 名命名，不按场景类型聚合
2. **Fixture per service** - 每个 service 一个 fixture，SetUp 组装所有 mock + ServiceImpl
3. **FakeClosure** - 测试用 `google::protobuf::Closure` 假实现（`tests/fixtures/fake_closure.hpp`），记录 `Run()` 调用次数
4. **Helper 函数** - `make_user()` / `make_message()` 等放 `tests/fixtures/proto_helpers.hpp`
5. **不测 brpc controller** - handler 内部如需 controller，传 `nullptr` 或 `FakeController`

### 3.5 CMakeLists.txt 模式

```cmake
# transmite/test/unit/CMakeLists.txt
add_executable(transmite_unit_tests
    test_transmite_new_message.cc
    test_transmite_validation.cc
    test_transmite_large_group.cc
    test_transmite_idempotency.cc
)
target_link_libraries(transmite_unit_tests
    transmite_lib
    gmock gtest gtest_main
    -lgflags -lprotobuf -lbrpc
)
gtest_discover_tests(transmite_unit_tests PROPERTIES LABELS "unit")
```

---

## 4. DAO 集成测试设计

### 4.1 docker-compose.test.yml

仅起基础设施，不起业务服务：

```yaml
# docker/docker-compose.test.yml
services:
  mysql:
    image: mysql:8.0
    environment:
      MYSQL_ROOT_PASSWORD: chatnow_test
      MYSQL_DATABASE: chatnow_test
    ports: ["3306:3306"]
    volumes:
      - ../sql:/docker-entrypoint-initdb.d
    healthcheck:
      test: ["CMD", "mysqladmin", "ping", "-h", "localhost"]
      interval: 3s
      retries: 30

  redis:
    image: redis:7-alpine
    ports: ["6379:6379"]

  elasticsearch:
    image: docker.elastic.co/elasticsearch/elasticsearch:7.17.0
    environment: { discovery.type: single-node }
    ports: ["9200:9200"]

  rabbitmq:
    image: rabbitmq:3-management
    ports: ["5672:5672"]

  minio:
    image: minio/minio
    command: server /data
    ports: ["9000:9000"]
    environment:
      MINIO_ROOT_USER: minioadmin
      MINIO_ROOT_PASSWORD: minioadmin
```

CI 用 `docker compose -f docker/docker-compose.test.yml up -d`；本地开发者也用同一文件。

### 4.2 环境变量门控

沿用现有 `MINIO_TEST=1` 模式，按依赖粒度控制：

| 环境变量 | 门控的测试 |
|---|---|
| `DB_TEST=1` | MySQL DAO 集成测试 |
| `REDIS_TEST=1` | Redis DAO 集成测试 |
| `ES_TEST=1` | Elasticsearch 集成测试 |
| `MQ_TEST=1` | RabbitMQ 集成测试 |
| `MINIO_TEST=1` | S3/MinIO 集成测试（已有） |

不设变量时 `GTEST_SKIP()`，本地开发不强制起全栈。CI 里全部设为 `1`。

### 4.3 测试数据隔离策略

**MySQL**：per-test-suite 共享连接，每个测试用 `TRUNCATE TABLE` 清空。不用事务回滚（ODB 事务和测试边界不匹配，且要测真实 commit 行为）。

```cpp
// tests/fixtures/db_fixture.hpp
class DBFixture : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        _db = make_mysql("127.0.0.1", "chatnow_test", "root", "chatnow_test");
    }
    void SetUp() override {
        odb::transaction t(_db->begin());
        _db->execute("TRUNCATE TABLE message");
        _db->execute("TRUNCATE TABLE user_timeline");
        _db->execute("TRUNCATE TABLE chat_session_member");
        t.commit();
    }
    static inline std::shared_ptr<odb::core::database> _db;
};
```

**ES**：每个测试 delete + recreate index。
**MQ**：每个测试用唯一 queue 名（`test_<uuid>`），测试后自动 delete queue。

### 4.4 Phase 1 DAO 集成测试清单

```
message/test/integration/
├── CMakeLists.txt
├── test_message_table.cc              # MessageTable：insert/select/update/delete
├── test_user_timeline_table.cc        # UserTimeLineTable：写扩散/游标拉取/未读计数
├── test_chat_session_member_table.cc  # ChatSessionMemberTable：成员查询/last_read 更新
├── test_es_message.cc                 # ESMessage：索引/检索/分页
└── test_message_db_consumer.cc        # MQ DB consumer 端到端（投消息 -> 验证落库）

file/test/integration/                 # 已有，补齐
├── test_s3_integration.cc             # 已有
├── test_media_dao_integration.cc      # 已有
├── test_media_file_table.cc           # MediaFileTable：insert/select/quota
├── test_media_blob_ref.cc             # MediaBlobRefTable：ref_count/dedup
└── test_media_user_quota.cc           # MediaUserQuotaTable：配额增减/溢出
```

### 4.5 测试结构示例

```cpp
// message/test/integration/test_user_timeline_table.cc
class UserTimelineTableTest : public DBFixture {
protected:
    UserTimeLineTable table{_db};
};

TEST_F(UserTimelineTableTest, WriteDiffusion_InsertsRowPerMember) {
    Message m = make_message(1001, "s1", "u1", "hello");
    ASSERT_TRUE(table.insert_for_members(m, {"u1", "u2", "u3"}));

    auto rows = table.select_by_user("u1", "s1");
    EXPECT_EQ(rows.size(), 1);
    EXPECT_EQ(rows[0].message_id(), 1001);
}

TEST_F(UserTimelineTableTest, OfflinePull_UsesCursor) {
    for (int64_t mid = 100; mid <= 104; ++mid) {
        table.insert_for_members(make_message(mid, "s1", "u1", "..."), {"u1"});
    }
    auto rows = table.select_after("u1", "s1", 102);
    EXPECT_EQ(rows.size(), 2);
    EXPECT_EQ(rows[0].message_id(), 103);
}

TEST_F(UserTimelineTableTest, UnreadCount_ComputesFromLastRead) {
    for (int64_t mid = 100; mid <= 104; ++mid) {
        table.insert_for_members(make_message(mid, "s1", "u1", "..."), {"u1"});
    }
    table.update_last_read("u1", "s1", 101);
    EXPECT_EQ(table.count_unread("u1", "s1"), 3);
}
```

### 4.6 MQ 集成测试模式

```cpp
// test_message_db_consumer.cc
class MessageDBConsumerTest : public MQFixture {
protected:
    void SetUp() override {
        MQFixture::SetUp();
        _queue = "test_db_consumer_" + uuid();
        _subscriber->declare_queue(_queue);
        _consumer = std::make_unique<DBConsumer>(_db, _subscriber, _queue);
        _consumer->start();
    }
    void TearDown() override {
        _consumer->stop();
        _subscriber->delete_queue(_queue);
    }
};

TEST_F(MessageDBConsumerTest, ConsumesMessage_WritesToDBAndTimeline) {
    InternalMessage msg = make_internal_message(2001, "s1", "u1", {"u1","u2"});
    _publisher->publish("msg_exchange", "", msg.SerializeAsString(), {});

    ASSERT_TRUE(wait_for([&] { return _msg_table->exists(2001); }, 5s));
    EXPECT_TRUE(_timeline_table->exists("u1", 2001));
    EXPECT_TRUE(_timeline_table->exists("u2", 2001));
}

TEST_F(MessageDBConsumerTest, DBFailure_NacksAndRequeues) {
    _msg_table->insert(make_message(2002, "s1", "u1", "x"));
    InternalMessage msg = make_internal_message(2002, "s1", "u1", {"u1"});

    _publisher->publish("msg_exchange", "", msg.SerializeAsString(), {});
    ASSERT_TRUE(wait_for([&] { return _dead_letter_count > 0; }, 10s));
}
```

---

## 5. 端到端测试设计

### 5.1 E2E 测试哲学

E2E 测试**不验证业务逻辑细节**（那是单元测试的职责），只验证**跨服务链路的正确性**：

- 数据是否从 A 服务流到 B 服务
- 协议层是否正确（HTTP 请求 -> WS 推送）
- 多服务协作下的数据一致性（transmite 投递 -> message 落库 -> ES 索引）
- 真实失败模式（服务重启后恢复、MQ 重投）

### 5.2 docker-compose 编排

E2E 用**现有的** `docker-compose.yml`（项目根目录），它已经能起全套基础设施 + 7 个业务服务。CI 里：

```yaml
# .github/workflows/ci.yml 的 e2e job
- run: docker compose up -d --build
- run: ./scripts/wait_for_services.sh
- run: ctest -L e2e --output-on-failure
- run: docker compose down -v
```

### 5.3 Phase 1 E2E 测试清单

```
tests/e2e/
├── CMakeLists.txt
├── test_message_pipeline.cc       # 发消息全链路
├── test_message_offline_sync.cc   # 离线消息同步
├── test_media_upload.cc           # 媒体三步上传全链路
└── helpers/
    ├── http_client.hpp            # 封装 cpp-httplib，带 JWT 注入
    ├── ws_client.hpp              # 封装 websocketpp，等通知
    └── test_users.hpp             # 预置用户注册/登录 fixture
```

### 5.4 测试场景设计

**场景 1：群消息全链路**（`test_message_pipeline.cc`）

```
预置：注册 u1, u2, u3；创建群会话 s1（含三人）
步骤：
  1. u1 登录 -> 拿 JWT + session_id
  2. u2, u3 登录 -> 各开 WS 连接
  3. u1 POST /service/message_transmit/new_message（发 "hello"）
  4. 验证 HTTP 响应 success=true，含 message_id
  5. 验证 u2, u3 的 WS 连接收到 CHAT_MESSAGE_NOTIFY
  6. 直查 DB：message 表有记录，user_timeline 有 3 行（写扩散）
  7. 直查 ES：message 索引有文档（文本消息）
  8. u2 GET /service/message_storage/recent_msg -> 返回 "hello"
  9. u2 GET /service/message_storage/unread_count -> 返回 1
  10. u2 ACK 未读 -> 再查 unread_count = 0
```

**场景 2：离线消息同步**（`test_message_offline_sync.cc`）

```
预置：u1, u2 是好友 + 单聊会话
步骤：
  1. u2 离线（不登录、不开 WS）
  2. u1 发 3 条消息
  3. u2 上线登录 -> GET GetOfflineMsg(last_message_id=0)
  4. 验证返回 3 条消息，按序号递增
  5. u2 开 WS -> 不应收到旧消息推送（已通过 offline 拉取）
  6. u1 再发 1 条 -> u2 WS 收到新通知
```

**场景 3：媒体三步上传**（`test_media_upload.cc`）

```
预置：u1 登录
步骤：
  1. POST /service/media/apply_upload（CHAT purpose, image/jpeg, 1024 bytes）
     -> 返回 file_id + presigned PUT URL
  2. PUT 到 MinIO（用 presigned URL）
  3. POST /service/media/complete_upload（file_id）
     -> 验证 success=true
  4. 直查 DB：media_file 有记录，media_blob_ref ref_count=1，media_user_quota +1024
  5. GET /service/media/get_download_url（file_id）
     -> 返回 presigned GET URL
  6. GET 到 MinIO -> 验证内容与上传一致
  7. 重复上传相同内容（content_hash 相同）-> 验证 dedup：ref_count++ 但不新增 blob
```

### 5.5 测试 helper 设计

```cpp
// tests/e2e/helpers/http_client.hpp
class HttpClient {
public:
    HttpClient(const std::string& base = "http://127.0.0.1:9000");
    void set_token(const std::string& jwt);
    nlohmann::json post(const std::string& path, const nlohmann::json& body);
    nlohmann::json get(const std::string& path);
};

// tests/e2e/helpers/ws_client.hpp
class WsClient {
public:
    WsClient(const std::string& url);
    void connect();
    bool wait_notify(int type, int timeout_ms, nlohmann::json* out = nullptr);
};

// tests/e2e/helpers/test_users.hpp
class TestUsers {
public:
    TestUsers();  // 注册 u1/u2/u3，各自登录拿 JWT
    const UserInfo& operator[](const std::string& uid);
};
```

### 5.6 E2E fixture

```cpp
class E2EFixture : public ::testing::Test {
protected:
    void SetUp() override {
        if (!std::getenv("E2E_TEST")) GTEST_SKIP() << "E2E_TEST!=1";
        _http.get("/health");
    }
    HttpClient _http;
};
```

### 5.7 CMakeLists.txt

```cmake
# tests/e2e/CMakeLists.txt
add_executable(e2e_tests
    test_message_pipeline.cc
    test_message_offline_sync.cc
    test_media_upload.cc
)
target_link_libraries(e2e_tests
    gtest gtest_main
    -lcpphttplib -lwebsocketpp -lboost_system -lssl -lcrypto
    -ljsoncpp -lcurl
)
target_include_directories(e2e_tests PRIVATE helpers/)
gtest_discover_tests(e2e_tests PROPERTIES LABELS "e2e"
    DISCOVERY_MODE PRE_TEST)
```

---

## 6. CI 工作流设计

### 6.1 单文件 workflow 结构

```yaml
# .github/workflows/ci.yml
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
      - uses: actions/cache@v4
        with:
          path: build
          key: build-unit-${{ runner.os }}-${{ hashFiles('**/CMakeLists.txt', '**/*.hpp') }}
      - run: sudo apt-get update && sudo apt-get install -y libgtest-dev libgmock-dev libbrpc-dev libprotobuf-dev protobuf-compiler libodb-dev libodb-mysql-dev libssl-dev libcurl4-openssl-dev libjsoncpp-dev libboost-all-dev
      - run: mkdir -p build && cd build && cmake .. && make -j$(nproc) transmite_unit_tests message_unit_tests common_tests
      - run: cd build && ctest -L unit --output-on-failure

  integration:
    needs: unit
    runs-on: ubuntu-22.04
    steps:
      - uses: actions/checkout@v4
      - run: sudo apt-get install -y <同上依赖>
      - run: docker compose -f docker/docker-compose.test.yml up -d
      - run: ./scripts/wait_for_infra.sh
      - run: mkdir -p build && cd build && cmake .. && make -j$(nproc)
      - env: { DB_TEST: "1", REDIS_TEST: "1", ES_TEST: "1", MQ_TEST: "1", MINIO_TEST: "1" }
        run: cd build && ctest -L integration --output-on-failure
      - if: always()
        run: docker compose -f docker/docker-compose.test.yml down -v

  e2e:
    needs: integration
    runs-on: ubuntu-22.04
    if: github.event_name == 'schedule' || (github.event_name == 'pull_request' && github.base_ref == 'main')
    steps:
      - uses: actions/checkout@v4
      - run: sudo apt-get install -y <同上依赖>
      - run: docker compose up -d --build
      - run: ./scripts/wait_for_services.sh
      - run: mkdir -p build && cd build && cmake .. && make -j$(nproc) e2e_tests
      - env: { E2E_TEST: "1" }
        run: cd build && ctest -L e2e --output-on-failure
      - if: always()
        run: docker compose down -v
```

### 6.2 关键设计决策

**1. job 串联 + 短路**

`unit -> integration -> e2e` 用 `needs` 串联。unit 失败不浪费 integration 的 5 分钟，integration 失败不浪费 e2e 的 10 分钟。

**2. E2E 触发条件**

```yaml
if: github.event_name == 'schedule'
    || (github.event_name == 'pull_request' && github.base_ref == 'main')
```

普通 feature 分支 PR 不跑 E2E，合并到 main 的 PR 才跑。nightly 兜底全量。

**3. 缓存策略**

- `build/` 目录按 `CMakeLists.txt` + `*.hpp` 哈希缓存，加速增量编译
- Docker layer 缓存靠 `docker compose` 的 volume 复用
- apt 依赖不缓存（Ubuntu runner 自带部分，装增量包 < 30s）

**4. 依赖安装**

CI 直接用 apt 装 C++ 依赖。不在 macOS runner 上跑（目标环境是 Linux）。

**5. 健康检查脚本**

```
# scripts/wait_for_infra.sh（新增）
轮询 mysql:3306 / redis:6379 / es:9200 / mq:5672 / minio:9000
超时 60s 则 exit 1

# scripts/wait_for_services.sh（新增）
轮询 gateway:9000 + 7 个业务服务端口
超时 120s 则 exit 1
```

### 6.3 本地开发对齐

```bash
# 跑单元测试（无需起依赖）
cd build && ctest -L unit

# 跑集成测试（先起基础设施）
docker compose -f docker/docker-compose.test.yml up -d
DB_TEST=1 REDIS_TEST=1 ES_TEST=1 MQ_TEST=1 MINIO_TEST=1 ctest -L integration

# 跑 E2E（起全套）
docker compose up -d
E2E_TEST=1 ctest -L e2e
```

CI 和本地命令完全一致，仅环境变量由 workflow 注入。

---

## 7. 分期实施计划

### 7.1 Phase 0：测试基础设施（无业务测试）

**目标**：搭好骨架，让后续业务测试有地方放、有 CI 跑、有 fixture 复用。

| # | 交付物 | 说明 |
|---|---|---|
| 0.1 | `docker/docker-compose.test.yml` | 仅基础设施（mysql/redis/es/mq/minio） |
| 0.2 | `scripts/wait_for_infra.sh` + `scripts/wait_for_services.sh` | 健康检查脚本 |
| 0.3 | `.github/workflows/ci.yml` | 三 job 串联 workflow |
| 0.4 | 根 `CMakeLists.txt` 接入 CTest | `enable_testing()` + 各 test 子目录 |
| 0.5 | `tests/mocks/` 目录 + 共享 mock 基类 | `mock_*.hpp` 存放位置约定 |
| 0.6 | `tests/fixtures/` 目录 | `db_fixture.hpp`、`mq_fixture.hpp`、`fake_closure.hpp`、`proto_helpers.hpp` |
| 0.7 | `tests/e2e/helpers/` 目录 | `http_client.hpp`、`ws_client.hpp`、`test_users.hpp` |
| 0.8 | transmite + message 的接口抽取 | `i_*.hpp` 接口文件 + ServiceImpl 改造持有接口 |

**验收**：CI 跑通空测试套件（unit/integration/e2e 各一个 dummy test），workflow 绿。

### 7.2 Phase 1：核心消息链路测试（transmite + message）

**目标**：覆盖 IM 命脉链路的单元 + DAO 集成 + E2E。

| # | 交付物 | 类型 | 文件数 |
|---|---|---|---|
| 1.1 | transmite 单元测试 | unit | 4 |
| 1.2 | message 单元测试 | unit | 6 |
| 1.3 | message DAO 集成测试 | integration | 5 |
| 1.4 | message MQ consumer 集成测试 | integration | 1（含在 1.3 文件清单） |
| 1.5 | file/media DAO 集成测试补齐 | integration | 3 |
| 1.6 | E2E：群消息全链路 | e2e | 1 |
| 1.7 | E2E：离线消息同步 | e2e | 1 |
| 1.8 | E2E：媒体三步上传 | e2e | 1 |

**验收**：
- unit 套件覆盖 transmite/message 所有 RPC handler 的正常路径 + 输入校验 + 依赖失败
- integration 套件覆盖 message 4 个表 + ES + MQ consumer
- E2E 套件 3 个场景在 nightly CI 绿
- 估计测试文件 22 个

### 7.3 Phase 2+：其他服务（后续 spec，本次不细化）

| Phase | 范围 | 触发条件 |
|---|---|---|
| Phase 2 | media 服务完整单元测试（UploadHandler/MultipartHandler/CleanupWorker） | Phase 1 验收后 |
| Phase 3 | gateway 单元测试（鉴权/路由/WS 连接管理） | Phase 2 验收后 |
| Phase 4 | friend + chatsession 单元测试 | Phase 3 验收后 |
| Phase 5 | push 服务测试 | 视需求 |

每个 Phase 独立 spec + plan，不在本次设计范围内。

### 7.4 风险与缓解

| 风险 | 缓解 |
|---|---|
| 接口抽取改动量大，引入回归 | Phase 0.8 先抽接口 + 跑现有 common/test 确保不破坏，再写新测试 |
| gmock 学习成本 | 提供一个完整示例（test_transmite_new_message.cc）作为模板，后续照抄 |
| E2E 在 CI 不稳定（docker 竞态） | wait_for_services.sh 轮询 + 超时 fail fast；E2E 只在 nightly + PR to main 跑 |
| MySQL TRUNCATE 慢 | 测试表数据量小（< 100 行），TRUNCATE < 10ms，可接受 |

---

## 8. 总结

本设计建立了 ChatNow 的三层测试体系：

1. **单元测试**（gtest + gmock）- ServiceImpl 业务逻辑，依赖通过接口 mock 隔离
2. **DAO 集成测试**（gtest + docker-compose）- 真实 MySQL/Redis/ES/MQ/MinIO，验证数据访问层
3. **端到端测试**（gtest + 全栈 docker-compose）- 跨服务链路 + 数据一致性

CI 用单 workflow 三 job 串联（unit -> integration -> e2e），本地与 CI 命令一致。Phase 0 搭基础设施，Phase 1 覆盖核心消息链路（transmite + message），后续 Phase 覆盖其余服务。

接口抽取是本设计对生产代码最大的改动：ServiceImpl 持有 `shared_ptr<I*>` 而非具体类型，接口与实现分文件存放（`i_*.hpp` + `<concrete>.hpp`），既提升可测试性又保持编译依赖清晰。
