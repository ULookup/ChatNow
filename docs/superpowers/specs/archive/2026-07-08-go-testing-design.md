# ChatNow 测试架构设计（Go 测试套件）

> **状态**: 设计完成，待评审
> **日期**: 2026-07-08
> **范围**: 全部测试统一为 Go（tests/func + tests/perf），移除 C++ 测试，建立 CI 自动化
> **基线**: 3.0-dev 现有 Go 测试套件（tests/func/ 2502 行 / tests/perf/ 233 行 / tests/pkg/ 294 行）
> **目标**: 纯 Go 测试栈 + CI 自动化 + 覆盖核心链路错误路径与数据一致性

---

## 0. 设计原则

1. **纯 Go 测试** - 所有测试用 Go 编写，不保留 C++ gtest 测试。C++ 侧仅保留生产代码。
2. **黑盒行为测试** - 通过 HTTP + protobuf 外部 API 验证服务行为，不测 C++ 内部实现。内部逻辑通过行为间接覆盖。
3. **复用现有设施** - tests/func/、tests/perf/、tests/pkg/ 已建立，在其上扩展而非另起炉灶。
4. **CI 驱动** - 测试必须能在 GitHub Actions 上自动跑，本地与 CI 命令一致。
5. **YAGNI** - 不引入额外测试框架，用 testify + Go 标准 testing。不 mock 服务，用真实全栈。

明确**不做**的事：
- ❌ 不保留 C++ gtest 单元测试（common/test/、media/test/ 全部移除）
- ❌ 不做 C++ 接口抽取（Go 黑盒测试不需要改生产代码）
- ❌ 不在 macOS runner 上跑 CI（目标环境是 Linux）
- ❌ 不引入 testcontainers（用 docker-compose 更简单）
- ❌ Phase 1 不新增 perf 测试（现有 4 个够用，先补功能覆盖）

---

## 1. 测试层次

```
┌──────────────────────────────────────────────────┐
│  L4  性能测试    tests/perf/         nightly      │
│      基准 + 回归阈值                              │
├──────────────────────────────────────────────────┤
│  L3  场景测试    tests/func/         PR to main   │
│      跨服务 E2E 链路 + 数据一致性断言              │
│      scenarios_test.go                           │
├──────────────────────────────────────────────────┤
│  L2  功能测试    tests/func/         每 PR        │
│      每服务 API 级测试 + 错误路径                 │
│      identity/conversation/message/...           │
└──────────────────────────────────────────────────┘
```

### 1.1 L2 功能测试（tests/func/）

**定位**：每服务独立的功能验证，通过 HTTP API 调用，验证请求/响应正确性 + 错误路径。

**运行方式**：需要全套 docker-compose（基础设施 + 业务服务），但每个测试文件独立，可并行。

**覆盖目标**：
- 正常路径（happy path）- 已有，补充薄弱服务
- 错误路径（invalid input / auth failure / quota exceeded / duplicate）- 当前缺失，重点补
- 边界条件（空列表 / 分页边界 / 超长字段）

### 1.2 L3 场景测试（tests/func/scenarios_test.go）

**定位**：跨服务 E2E 链路，模拟真实用户旅程，验证多服务协作 + 数据一致性。

**与 L2 的区别**：
- L2 验证单个 API 的行为
- L3 验证多个 API 串联的链路正确性 + 跨服务数据一致性（如发消息后直查 DB/ES）

**已有场景**（3 个）：
1. `TestScenario_RegisterToFirstMessage` - 注册->登录->加好友->发消息->同步->历史
2. `TestScenario_GroupChatLifecycle` - 建群->发图->@mention->reaction->撤回->解散
3. `TestScenario_FriendFullLifecycle` - 加好友->聊天->删好友->验证好友列表

**需补充场景**：
4. 离线消息同步（用户离线->收消息->上线拉取->WS 实时推送）
5. 媒体三步上传全链路（apply->PUT->complete->download->dedup）
6. 消息可靠性与重试（MQ 投递失败->重投->最终落库）

### 1.3 L4 性能测试（tests/perf/）

**定位**：基准测试，验证吞吐/延迟不退化。

**已有场景**（4 个）：login、send_msg、sync、upload。

**运行频率**：nightly only，避免拖慢 PR。

---

## 2. 现有覆盖评估

### 2.1 功能测试覆盖度（tests/func/）

| 文件 | 行数 | 覆盖评估 | 缺口 |
|---|---|---|---|
| `identity_test.go` | 534 | 较完整 | 缺错误路径（重复注册/错误密码/过期验证码） |
| `conversation_test.go` | 460 | 较完整 | 缺权限边界（非成员操作/转让群主） |
| `transmite_test.go` | 422 | 较完整 | 缺大群读扩散分支/限流/幂等去重 |
| `message_test.go` | 348 | 中等 | 缺 ES 检索分页/未读计数边界/批量删除 |
| `relationship_test.go` | 225 | 中等 | 缺拉黑/申请超时/重复申请 |
| `presence_test.go` | 116 | **薄** | 缺多设备/心跳续期/离线推送 |
| `media_test.go` | 95 | **很薄** | 缺三步上传/multipart/dedup/quota/cleanup |
| `auth_middleware_test.go` | 79 | 中等 | 缺 token 刷新/黑名单/多设备踢 |
| `scenarios_test.go` | 205 | 3 场景 | 需补离线同步/媒体上传/可靠性场景 |

### 2.2 共享设施评估（tests/pkg/）

| 文件 | 评估 | 缺口 |
|---|---|---|
| `client/http.go` | 完善（protobuf over HTTP + JWT 注入） | 缺 WebSocket 客户端（presence/实时推送验证需要） |
| `client/config.go` | 完善（YAML + env 覆盖） | 无 |
| `fixture/auth.go` | RegisterAndLogin helper | 缺多用户批量注册/群组创建 helper |
| `fixture/friend.go` | 好友建立 helper | 无 |
| `fixture/conversation.go` | 会话建立 helper | 无 |

### 2.3 C++ 测试清单（待移除）

| C++ 测试文件 | 测什么 | Go 行为测试映射 |
|---|---|---|
| `common/test/test_mime_whitelist.cc` | C++ MimeWhitelist 类 | media_test.go: 上传不同 mime 验证接受/拒绝 |
| `common/test/test_jwt_codec.cc` | JWT 编解码 | auth_middleware_test.go: 登录/过期/无效 token |
| `common/test/test_jwt_store.cc` | JWT 存储/黑名单 | auth_middleware_test.go: token 刷新/踢 |
| `common/test/test_content_hash.cc` | 内容哈希 | media_test.go: 重复上传验证 dedup |
| `common/test/test_object_key.cc` | 对象 key 生成 | media_test.go: 上传后验证 MinIO key |
| `common/test/test_magic_sniff.cc` | magic number 嗅探 | media_test.go: mime 不匹配验证拒绝 |
| `common/test/test_auth_context.cc` | auth context | auth_middleware_test.go（已有） |
| `common/test/test_forward_auth.cc` | 转发鉴权 | auth_middleware_test.go（已有） |
| `common/test/test_service_error.cc` | 错误码 | 各服务错误路径测试 |
| `common/test/test_trace_id.cc` | trace_id 生成 | scenarios: 验证响应 header trace_id |
| `common/test/test_mq_trace_headers.cc` | MQ trace 透传 | scenarios: 链路 trace 一致性 |
| `common/test/test_log_context.cc` | 日志上下文 | 不迁移（实现细节，无行为可测） |
| `common/test/test_log_json.cc` | 日志 JSON 格式 | 不迁移（实现细节） |
| `common/test/test_avatar_url.cc` | 头像 URL 格式 | identity_test.go: 设置头像验证 URL |
| `common/test/test_mysql_user_block_compile.cc` | 编译测试 | 不迁移（无行为） |
| `media/test/test_s3_integration.cc` | MinIO 集成 | media_test.go: 三步上传全链路 |
| `media/test/test_media_dao_integration.cc` | media DAO | media_test.go: 上传 + 直查 DB |
| `identity/test/identity_client.cc` | 测试客户端 | 已被 tests/pkg/client/ 取代 |

---

## 3. CI 工作流设计

### 3.1 单文件 workflow

```yaml
# .github/workflows/ci.yml
name: CI
on:
  push:
    branches: [main, develop, 3.0-dev]
  pull_request:
    branches: [3.0-dev]
  schedule:
    - cron: "0 2 * * *"   # nightly

jobs:
  func:
    runs-on: ubuntu-22.04
    steps:
      - uses: actions/checkout@v4
      - uses: actions/setup-go@v5
        with: { go-version: '1.23' }
      - run: docker compose up -d --build
      - run: ./scripts/wait_for_services.sh
      - run: cd tests && make proto
      - run: cd tests && make test-func
      - if: always()
        run: docker compose down -v

  scenario:
    needs: func
    runs-on: ubuntu-22.04
    if: github.event_name == 'schedule' || (github.event_name == 'pull_request' && github.base_ref == '3.0-dev')
    steps:
      - uses: actions/checkout@v4
      - uses: actions/setup-go@v5
        with: { go-version: '1.23' }
      - run: docker compose up -d --build
      - run: ./scripts/wait_for_services.sh
      - run: cd tests && make proto
      - run: cd tests && make test-scenario
      - if: always()
        run: docker compose down -v

  perf:
    needs: scenario
    runs-on: ubuntu-22.04
    if: github.event_name == 'schedule'
    steps:
      - uses: actions/checkout@v4
      - uses: actions/setup-go@v5
        with: { go-version: '1.23' }
      - run: docker compose up -d --build
      - run: ./scripts/wait_for_services.sh
      - run: cd tests && make proto
      - run: cd tests && make test-perf
      - if: always()
        run: docker compose down -v
```

### 3.2 触发策略

| Job | 触发 | 理由 |
|---|---|---|
| `func` | 每 push + PR | 快速反馈，~5min |
| `scenario` | PR to 3.0-dev + nightly | 较慢，合并前跑 |
| `perf` | nightly only | 最慢，防退化 |

### 3.3 健康检查脚本

新增 `scripts/wait_for_services.sh`（3.0-dev 当前无此脚本）：
- 轮询 gateway:9000 + 8 个业务服务端口
- 超时 120s 则 exit 1
- 复用现有 docker-compose.yml 的 healthcheck

### 3.4 本地与 CI 一致

```bash
# 本地跑功能测试
docker compose up -d
cd tests && make proto && make test-func

# 本地跑场景测试
cd tests && make test-scenario

# 本地跑性能测试
cd tests && make test-perf
```

CI 仅多一步 `docker compose up` + `wait_for_services.sh`。

---

## 4. docker-compose.test.yml

**不新增**。现有 `docker-compose.yml` 已包含全套基础设施 + 业务服务，测试直接复用。

不单独搞"仅基础设施"的 compose，因为 Go 功能测试需要业务服务在线（通过 HTTP API 调用）。

---

## 5. 分期实施计划

### Phase 0：CI 基础设施（让现有测试自动跑）

**目标**：现有 Go 测试能在 GitHub Actions 上自动执行。

| # | 交付物 | 说明 |
|---|---|---|
| 0.1 | `scripts/wait_for_services.sh` | 服务健康检查脚本 |
| 0.2 | `.github/workflows/ci.yml` | 三 job 串联 workflow |
| 0.3 | `tests/Makefile` 补强 | 确保 `make proto` + `make test-func` 在 CI 环境可跑 |
| 0.4 | `tests/.gitignore` 确认 | 生成的 proto 代码不误提交 |

**验收**：PR 触发 func job，现有 10 个功能测试文件全绿。

### Phase 1：核心消息链路覆盖补齐

**目标**：补齐 transmite + message 的错误路径 + 数据一致性断言。

| # | 交付物 | 类型 |
|---|---|---|
| 1.1 | transmite 错误路径测试 | L2 func |
| 1.2 | message 错误路径测试 | L2 func |
| 1.3 | 数据一致性断言 helper | tests/pkg/ |
| 1.4 | 离线消息同步场景 | L3 scenario |
| 1.5 | 消息可靠性场景 | L3 scenario |

**验收**：消息链路的错误路径（无效消息类型/缺 file_id/超限/重复发送）有测试覆盖；场景测试直查 DB 验证 message 表 + user_timeline 写扩散。

### Phase 2：media + presence 覆盖 + C++ 测试移除

**目标**：补齐薄弱服务覆盖，移除全部 C++ 测试。

| # | 交付物 | 类型 |
|---|---|---|
| 2.1 | media 三步上传完整测试 | L2 func |
| 2.2 | media multipart/dedup/quota 测试 | L2 func |
| 2.3 | presence 多设备/心跳测试 | L2 func |
| 2.4 | 媒体上传 E2E 场景 | L3 scenario |
| 2.5 | 移除 common/test/ C++ 测试 | 清理 |
| 2.6 | 移除 media/test/ C++ 测试 | 清理 |
| 2.7 | 移除 identity/test/ C++ 测试 | 清理 |
| 2.8 | 根 CMakeLists.txt 移除 common/test 子目录 | 清理 |

**验收**：media_test.go 从 95 行扩展到覆盖三步上传/multipart/dedup/quota/cleanup；所有 C++ 测试文件删除；CMake 不再构建任何 test target。

### Phase 3：性能基线 + 回归阈值（后续 spec）

nightly perf 测试建立基线，设定回归阈值（如吞吐下降 >10% 则 fail）。

---

## 6. 风险与缓解

| 风险 | 缓解 |
|---|---|
| CI 里 docker compose 起全栈慢（~2min） | func job 并行跑测试文件；cache Go 模块 |
| C++ 测试移除后丢失覆盖 | Phase 2 移除前确保 Go 行为测试已覆盖同等行为 |
| Go protobuf 生成依赖 protoc | Makefile 的 `make proto` 目标已处理；CI 装 protobuf-compiler |
| 现有 func 测试偏 happy path | Phase 1 重点补错误路径 |
| WebSocket 客户端缺失（presence/推送验证） | Phase 2 补 tests/pkg/client/ws.go |

---

## 7. 总结

本设计将 ChatNow 测试统一为纯 Go 栈：

1. **L2 功能测试**（tests/func/）- 每服务 API 级测试 + 错误路径
2. **L3 场景测试**（tests/func/scenarios_test.go）- 跨服务 E2E + 数据一致性
3. **L4 性能测试**（tests/perf/）- 基准 + 回归

CI 三 job 串联（func -> scenario -> perf），本地与 CI 命令一致。Phase 0 搭 CI，Phase 1 补消息链路覆盖，Phase 2 补 media/presence + 移除 C++ 测试，Phase 3 性能基线。

与之前 C++ gtest 方案的根本区别：**不改任何生产代码**（无接口抽取），Go 黑盒测试通过外部 API 验证行为。C++ 测试全部移除，测试栈统一为 Go。
