# ChatNow 测试体系优化设计

## 目标

建立四层递进测试体系，从纯逻辑到端到端，覆盖 ChatNow 8 个服务的核心路径。

## 现有问题

1. **C++ 公共库单测全部无法编译**：15 个测试文件的 CMakeLists.txt 被注释，根因是 gflags/brpc 静态库链接符号冲突
2. **服务级 C++ 测试完全缺失**：8 个服务的 ~60 个 RPC handler + MQ consumer 零 C++ 单测
3. **Go 集成测试有盲区**：Gateway、Push、Media(部分) 无集成测试覆盖
4. **5/19 测试设计文档计划的 7 个新单测未创建**

## 方案：四层递进

### L1：公共库纯逻辑单测（22 文件，~79 用例）

**修复编译**：调整 `common/test/CMakeLists.txt` 中 `target_link_libraries` 顺序（`-lbrpc` 置于 `-lgflags` 之前），按依赖拆分编译目标（无依赖 vs brpc 依赖 vs Redis 依赖）。

**已有文件激活（15 个）**：

| 文件 | 依赖 | 用例(估) |
|------|------|---------|
| test_jwt_codec.cc | 无 | 8 |
| test_jwt_store.cc | Redis | 6 |
| test_auth_context.cc | brpc | 5 |
| test_forward_auth.cc | brpc | 4 |
| test_service_error.cc | 无 | 5 |
| test_trace_id.cc | 无 | 3 |
| test_log_context.cc | 无 | 4 |
| test_log_json.cc | 无 | 3 |
| test_avatar_url.cc | 无 | 3 |
| test_content_hash.cc | 无 | 4 |
| test_object_key.cc | 无 | 3 |
| test_magic_sniff.cc | 无 | 4 |
| test_mime_whitelist.cc | 无 | 4 |
| test_mq_trace_headers.cc | 无 | 3 |
| test_mysql_user_block_compile.cc | ODB | 占位 |

**新增文件（7 个）**：

| 文件 | 测试目标 | 依赖 | 用例 |
|------|---------|------|------|
| test_bcrypt_util.cc | hash_password / check_password | 无 | 4 |
| test_random_ttl.cc | range [base*0.75, base*1.25] | 无 | 3 |
| test_handle_rpc.cc | metadata 提取、缺字段抛错 | brpc | 4 |
| test_inflight.cc | 并发 key 保护、Guard RAII | std::thread | 5 |
| test_redis_mutex.cc | try_lock / TTL 释放 / unlock 幂等 | Redis | 5 |
| test_snowflake.cc | worker_id 唯一性、SeqGen 单调 | mock | 3 |
| test_leader_election.cc | campaign/stop 状态变迁 | etcd/mock | 3 |

### L2：服务级纯逻辑提取单测（6 文件，~45 用例）

只测已有 static 方法或参数自包含的成员函数，不做重构提取。

| 服务 | 文件 | 测试函数 | 用例 |
|------|------|---------|------|
| Identity | identity/test/test_identity_pure.cc | nickname_check, password_check, mail_check, fill_user_info | 14 |
| Conversation | conversation/test/test_conversation_pure.cc | private_id_of_, _to_ms, fill_self_member_info_, avatar_url_of_ | 12 |
| Message | message/test/test_message_pure.cc | convert_db_message_to_proto_, now_ms_, reaction 分组 | 10 |
| Transmite | transmite/test/test_transmite_pure.cc | 大群门限判断, 消息类型 file_id 校验 | 4 |
| Presence | presence/test/test_presence_pure.cc | 状态排名, 设备 TTL 过期 | 5 |
| Relationship | relationship/test/test_relationship_pure.cc | 72h 拒绝窗口 | 2 |

private member 通过 `#define private public` 或 friend test class 暴露。

### L3：关键路径 C++ 集成测试（5 文件，~24 用例）

使用真实 Redis (db=15) 和真实 MySQL，PR 触发。

| 路径 | 文件 | 关键场景 | 用例 | 依赖 |
|------|------|---------|------|------|
| RefreshToken 重放 | identity/test/test_refresh_token_integration.cc | 正常滚动、重放检测、黑名单拦截、过期、跨设备、Logout 清理 | 6 | Redis |
| Transmite 幂等 | transmite/test/test_idempotency.cc | 首次、重复命中、并发冲突(pending)、投递失败清理 | 4 | Redis |
| Push 路由解析 | push/test/test_route_resolve.cc | L1 miss→L2 hit、L1 hit、Inflight 并发、全部 miss | 4 | Redis |
| Presence 聚合 | presence/test/test_presence_aggregation.cc | 单设备、多设备取最佳、全 INVISIBLE→OFFLINE、TTL 过期、无设备 | 5 | Redis |
| Message DB Consumer | message/test/test_db_consumer.cc | TEXT 落库+timeline、重复幂等、大群跳过 timeline、非文本不写 ES、反序列化失败 | 5 | MySQL |

### L4：Go 集成测试补缺（4 文件，~12 用例）

| 文件 | 用例 | 依赖 |
|------|------|------|
| gateway_test.go | HealthCheck, WS 带 token, WS 无 token, JWT 中间件拦截 | 服务栈 |
| push_test.go | RegisterDevice, UnregisterDevice, 在线设备收到推送 | 服务栈+WS client |
| media_test.go (补充) | CompleteUpload 状态变迁, GetDownloadUrl, 配额超限 | 服务栈+MinIO |
| scenarios_test.go (补充) | Token 过期刷新, 被拉黑后消息拦截 | 服务栈 |

`push_test.go` 需要 Go 侧 WebSocket 客户端能力——需在 `tests/pkg/client/` 增加 WS 封装。

## 目录结构

```
common/test/                            # L1
├── CMakeLists.txt                      #   修复编译
├── test_jwt_codec.cc                   #   已有 x15
├── test_bcrypt_util.cc                 #   新增 x7
├── test_random_ttl.cc
├── test_handle_rpc.cc
├── test_inflight.cc
├── test_redis_mutex.cc
├── test_snowflake.cc
└── test_leader_election.cc

identity/test/                          # L2+L3
├── test_identity_pure.cc
└── test_refresh_token_integration.cc

conversation/test/                      # L2
└── test_conversation_pure.cc

message/test/                           # L2+L3
├── test_message_pure.cc
└── test_db_consumer.cc

transmite/test/                         # L2+L3
├── test_transmite_pure.cc
└── test_idempotency.cc

presence/test/                          # L2+L3
├── test_presence_pure.cc
└── test_presence_aggregation.cc

push/test/                              # L3
└── test_route_resolve.cc

tests/func/                             # L4
├── gateway_test.go
├── push_test.go
├── media_test.go                       #   补充
└── scenarios_test.go                   #   补充
```

## CI 集成

| 触发时机 | 内容 | 环境要求 |
|---------|------|---------|
| 每次 commit | L1 无依赖(~60) + L2 全部(~45) + 编译检查 | 无 |
| PR | L1 Redis(~19) + L3 全部(~24) + L4 全部(~97) | Redis + MySQL + Docker |
| Daily | 全量测试 + Go 场景 + 性能测试(tests/perf/) | 全栈 |

## 测试运行命令

```bash
# L1 无依赖
./common_tests --gtest_filter=-*Redis*:*Etcd*

# L1 Redis
./common_tests --gtest_filter=*Redis*:*Etcd*

# L2 (各服务独立 target)
./identity_pure_tests
./conversation_pure_tests
./message_pure_tests
./transmite_pure_tests
./presence_pure_tests

# L3 (各服务独立 target)
./identity_integration_tests
./transmite_integration_tests
./push_integration_tests
./presence_integration_tests
./message_integration_tests

# L4
make -C tests test-func
```

## 不在范围

- Mock 框架（ODB/Redis/etcd/brpc 的 mock 层不做）
- identity/test/identity_client.cc 修复（过期 pre-3.0 API，由 Go 集成测试替代）
- 性能测试扩展（tests/perf/ 保持现状）
- 代码覆盖率门禁（下一步再做）
