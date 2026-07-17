# ChatNow 各服务单元测试设计

## 概述

为 ChatNow 项目设计两层测试体系：
- **C++ 纯单元测试**：覆盖 common 公共库纯逻辑组件，无外部依赖或仅需 Redis/etcd
- **Go 集成测试**：通过 HTTP Gateway 调用 C++ 服务，验证端到端 RPC 行为

## 现有测试现状

### Go 集成测试（`tests/func/`）— 已有 7 个服务覆盖

| 服务 | 覆盖 RPC | 测试用例数 |
|------|---------|-----------|
| Identity | Register/Login/Logout/RefreshToken/SendVerifyCode/GetProfile/UpdateProfile/GetMultiUserInfo/SearchUsers + 鉴权中间件 | ~15 |
| Transmite | SendMessage 所有类型 + 幂等性 + 错误场景 | ~14 |
| Message | Sync/GetHistory/GetById/Search/Recall/AddReaction/RemoveReaction/GetReactions/Pin/Unpin/ListPinned/Delete/Clear | ~14 |
| Conversation | List/Get/Create/Update/AddMembers/RemoveMembers/TransferOwner/Dismiss/ChangeRole/ListMembers/SetMute/SetPin/SetVisible/Quit/MarkRead/SaveDraft/Search | ~17 |
| Relationship | ListFriends/SendFriendReq/HandleFriendReq/RemoveFriend/Block/Unblock/ListBlocked/ListPending/SearchFriends | ~12 |
| Presence | Get/BatchGet/Subscribe/Unsubscribe | ~4 |
| Media | ApplyUpload/ApplyDownload/GetFileInfo/SpeechRecognition | ~6 |
| E2E Scenarios | 注册到首条消息、群聊生命周期、好友完整生命周期 | 3 |

### C++ 公共库单测（`common/test/`）— 代码已写，CMake 已注释

15 个测试文件存在但无法编译：jwt_codec、jwt_store、auth_context、forward_auth、service_error、trace_id、log_context、log_json、avatar_url、content_hash、object_key、magic_sniff、mime_whitelist、mq_trace_headers、mysql_user_block_compile(占位)。

### 服务级 C++ 测试

- `identity/test/` — 过期集成测试，CMake 已注释
- `media/test/` — DAO 集成测试（DB_TEST=1 可用）
- `push/test/` — 空目录
- `transmite/test/` — 空目录

## Part A：C++ 公共库纯单元测试

### A1. 修复 `common/test/CMakeLists.txt`

**问题**：gflags 与 brpc 静态库链接顺序导致符号冲突，整段 CMake 被注释。

**修改**：调整 `target_link_libraries` 顺序，`-lbrpc` 置于 `-lgflags` 之前，或使用 `-Wl,--whole-archive` 包裹 brpc 静态库。

**收益**：激活 15 个已有测试文件，立即获得覆盖。

### A2. 新增单测文件（7 个，按优先级排序）

| 优先级 | 文件 | 测试目标 | 外部依赖 | 预计用例 |
|-------|------|---------|---------|---------|
| P0 | `test_bcrypt_util.cc` | bcrypt_util: hash_password 产生可验证哈希、check_password 正确比对、空密码边界 | 无 | 4 |
| P0 | `test_random_ttl.cc` | randomized_ttl: 多次调用输出在 [base*0.75, base*1.25]、zero base 行为 | 无 | 3 |
| P1 | `test_handle_rpc.cc` | HANDLE_RPC 宏: 正常提取 auth、缺 metadata 抛错、缺 trace_id 自动生成 | brpc::Controller | 4 |
| P1 | `test_inflight.cc` | InflightRegistry: 并发同一 key 仅一个飞行请求、等待者获得相同结果、Guard RAII 析构自动 release | 无, 用 std::thread | 5 |
| P1 | `test_redis_mutex.cc` | RedisMutex: try_lock 成功后重复尝试失败、ttl 到期自动释放、unlock 幂等 | Redis（127.0.0.1:6379 db=15） | 5 |
| P2 | `test_snowflake.cc` | Snowflake: worker_id 分配唯一性（mock EtcdWorkIdAllocator）、SeqGen 序列号单调递增 | 无（mock） | 3 |
| P2 | `test_leader_election.cc` | LeaderElection: is_leader 初始状态、campaign 后 is_leader=true、stop 后 is_leader=false | etcd 或 mock | 3 |

### A3. 构建集成

在顶层 CMakeLists.txt 添加 `add_subdirectory(common/test)` 并确保 CI 中 `make common_tests && ./common_tests`。

## Part B：Go 集成测试补缺

### B1. 新增 `gateway_test.go`

- `TestHealthCheck` — GET `/health` 返回 200
- `TestWebSocketConnect_WithToken` — 携带有效 JWT 建立 WebSocket 连接成功
- `TestWebSocketConnect_NoToken` — 无 token 拒绝连接
- `TestJWTRequired_ConversationEndpoint` — 无 token 访问需要鉴权的 conversation API 被拒绝

### B2. 新增 `push_test.go`

- `TestRegisterDevice_Success` — 注册设备 token 成功
- `TestUnregisterDevice_Success` — 注销设备成功
- `TestSendPush_ToOnlineDevice` — 在线设备收到推送通知

### B3. 补充 `media_test.go`

- `TestConfirmUpload_Success` — 上传完成确认，文件状态从 PENDING 变为 ACTIVE
- `TestGetDownloadUrl_Success` — 获取下载 URL，返回有效签名链接
- `TestApplyUpload_QuotaExceeded` — 配额超限拒绝上传

## 测试运行策略

| 层级 | 运行方式 | CI 触发 | 环境要求 |
|------|---------|---------|---------|
| C++ 无依赖单测 | `./common_tests --gtest_filter=-*Redis*:*Etcd*` | 每次 commit | 无 |
| C++ Redis 依赖单测 | `./common_tests --gtest_filter=*Redis*:*Etcd*` | PR | Redis 127.0.0.1:6379 |
| Go 集成测试 | `make -C tests test-func` | PR / Daily | 完整服务栈 |
| Go 场景测试 | `make -C tests test-scenario` | Daily | 完整服务栈 |

## 不在范围内

- `identity/test/identity_client.cc` — 已过期（引用 pre-3.0 API），不修复，由 Go 集成测试替代
- `media/test/` DAO 集成测试 — 保持现状，不扩容
- 性能测试 — `tests/perf/` 已存在，本次不扩展
