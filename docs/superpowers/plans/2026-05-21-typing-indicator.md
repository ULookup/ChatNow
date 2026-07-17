# Typing 通知 — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 完成"对方正在输入..."服务端链路，使 PRIVATE 会话中 Client A 调用 SendTyping 后 Client B 通过 WebSocket 收到 TYPING_NOTIFY。

**Architecture:** 改造 Presence 服务的 SendTyping RPC：在现有 Redis 写入后新增推送逻辑——解析 PRIVATE 会话 ID 提取对方 uid，构造 NotifyTyping，通过 PushService::PushToUser 下发。Gateway 新增路由，OpenAPI 补充文档。Push 服务、Proto 均无需改动。

**Tech Stack:** C++ (brpc), Redis (sw::redis++), Protocol Buffers, YAML (OpenAPI 3.1)

**Spec:** `docs/superpowers/specs/2026-05-21-typing-indicator-design.md`

---

### Task 1: Presence 服务 SendTyping 增加推送下发逻辑

**Files:**
- Modify: `presence/source/presence_server.h:275-310`

- [ ] **Step 1: 改造 SendTyping 方法**

将 `SendTyping` 方法改为以下内容（替换现有 275-310 行）：

```cpp
void SendTyping(::google::protobuf::RpcController* base_cntl,
                const TypingReq* req, TypingRsp* rsp,
                ::google::protobuf::Closure* done) override
{
    brpc::ClosureGuard done_guard(done);
    auto* cntl = static_cast<brpc::Controller*>(base_cntl);
    try {
        auto auth = ::chatnow::auth::extract_auth(cntl);
        auto* h = rsp->mutable_header();
        h->set_success(true);
        h->set_error_code(::chatnow::error::kOK);
        h->set_request_id(req->request_id());

        const auto& conv_id = req->conversation_id();

        if (req->is_typing()) {
            auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            _redis->sadd("im:presence:typing:" + conv_id,
                         auth.user_id + ":" + std::to_string(now_ms));
            _redis->expire("im:presence:typing:" + conv_id, std::chrono::seconds(5));
        } else {
            std::vector<std::string> members;
            _redis->smembers("im:presence:typing:" + conv_id,
                             std::inserter(members, members.end()));
            for (const auto& m : members) {
                if (m.find(auth.user_id + ":") == 0) {
                    _redis->srem("im:presence:typing:" + conv_id, m);
                }
            }
        }

        // 仅 PRIVATE 会话下发 typing 通知
        if (conv_id.size() < 2 || conv_id[0] != 'p' || conv_id[1] != '_') return;

        auto pos = conv_id.find('_', 2);  // 第二个下划线，分隔 lo 和 hi
        if (pos == std::string::npos) return;

        std::string lo = conv_id.substr(2, pos - 2);
        std::string hi = conv_id.substr(pos + 1);
        std::string target = (lo == auth.user_id) ? hi : lo;

        auto channel = _channels->choose(_push_service_name);
        if (!channel) return;

        ::chatnow::push::NotifyMessage notify;
        notify.set_notify_type(::chatnow::push::NotifyType::TYPING_NOTIFY);
        auto* tn = notify.mutable_typing();
        tn->set_user_id(auth.user_id);
        tn->set_conversation_id(conv_id);
        tn->set_is_typing(req->is_typing());

        auto* closure = new SelfDeleteRpcClosure<::chatnow::push::PushToUserReq,
                                                  ::chatnow::push::PushToUserRsp>();
        closure->req.set_user_id(target);
        closure->req.mutable_notify()->CopyFrom(notify);

        ::chatnow::push::PushService_Stub stub(channel.get());
        stub.PushToUser(&closure->cntl, &closure->req, &closure->rsp, closure);
    } catch (const ServiceError& e) {
        rsp->mutable_header()->set_success(false);
        rsp->mutable_header()->set_error_code(e.code());
        rsp->mutable_header()->set_error_message(e.message());
        rsp->mutable_header()->set_request_id(req->request_id());
    }
}
```

注意变更：
- `expire` 从 10s 改为 5s
- 从 `notify_subscribers` 模式（include guard、日志级别）保持一致
- 仅 `p_` 前缀的 PRIVATE 会话 ID 下发通知
- 使用 `SelfDeleteRpcClosure` 做 fire-and-forget RPC 推送

- [ ] **Step 2: 编译验证**

```bash
cd build && cmake --build . --target presence_server 2>&1 | head -30
```

Expected: 编译通过，无错误。

- [ ] **Step 3: Commit**

```bash
git add presence/source/presence_server.h
git commit -m "feat(presence): add typing notify push to SendTyping"
```

---

### Task 2: Gateway 注册 SendTyping 路由

**Files:**
- Modify: `gateway/source/gateway_server.h:425`（在 UnsubscribePresence 路由之后插入）

- [ ] **Step 1: 添加路由**

在 `gateway/source/gateway_server.h` 第 425 行（`UnsubscribePresence` 路由之后）插入：

```cpp
    route<pres::PresenceService_Stub, pres::TypingReq, pres::TypingRsp>(
        "/service/presence/send_typing", _presence_svc, GatewayAuth::JWT_REQUIRED,
        &pres::PresenceService_Stub::SendTyping);
```

注意：需要确认 `pres::TypingReq` 和 `pres::TypingRsp` 在 proto 生成的 `presence_service.pb.h` 中已定义（它们已在 proto 文件中定义，编译时生成）。

- [ ] **Step 2: 编译验证**

```bash
cd build && cmake --build . --target gateway_server 2>&1 | head -30
```

Expected: 编译通过，无错误。

- [ ] **Step 3: Commit**

```bash
git add gateway/source/gateway_server.h
git commit -m "feat(gateway): add SendTyping route for presence service"
```

---

### Task 3: OpenAPI 文档更新

**Files:**
- Modify: `docs/api/openapi-presence.yaml`

- [ ] **Step 1: 添加 SendTyping 路径**

在 `paths` 下 `/service/presence/unsubscribe:` 之后新增：

```yaml
  /service/presence/send_typing:
    post:
      summary: 发送输入状态
      description: user_id 从 JWT metadata 提取。仅 PRIVATE 会话生效，GROUP/CHANNEL 静默忽略。
      tags: [Presence]
      x-protobuf: { service: chatnow.presence.PresenceService, rpc: SendTyping, request: TypingReq, response: TypingRsp, file: proto/presence/presence_service.proto }
      requestBody:
        required: true
        content:
          application/x-protobuf:
            schema: { $ref: '#/components/schemas/TypingReq' }
      responses:
        '200': { $ref: '#/components/responses/Success' }
        '4xx': { $ref: '#/components/responses/BusinessError' }
```

- [ ] **Step 2: 添加 TypingReq Schema**

在 `components/schemas` 部分的末尾（`UnsubscribeReq` 之后）新增：

```yaml
    TypingReq:
      x-protobuf: { message: TypingReq, file: proto/presence/presence_service.proto }
      type: object
      required: [request_id, conversation_id, is_typing]
      properties:
        request_id: { type: string }
        conversation_id: { type: string }
        is_typing: { type: boolean }
```

- [ ] **Step 3: 验证 YAML 合法性**

```bash
python3 -c "import yaml; yaml.safe_load(open('docs/api/openapi-presence.yaml'))" && echo "YAML valid"
```

Expected: `YAML valid`

- [ ] **Step 4: Commit**

```bash
git add docs/api/openapi-presence.yaml
git commit -m "docs(api): add SendTyping endpoint to openapi-presence"
```

---

### Task 4: 功能验证

**Files:**
- No file changes, manual verification only

- [ ] **Step 1: 确认编译通过**

```bash
cd build && cmake --build . --target presence_server gateway_server 2>&1 | tail -10
```

Expected: 两个 target 均编译成功。

- [ ] **Step 2: 路由注册验证**

```bash
grep -A2 "send_typing" gateway/source/gateway_server.h
```

Expected: 显示完整 route 调用，路径为 `/service/presence/send_typing`。

- [ ] **Step 3: 伪代码走查**

在 `presence/source/presence_server.h` 的 SendTyping 中确认：
- PRIVATE 会话 ID `p_uid1_uid2` → 正确解析出 lo/hi 并选择非 caller 的 target
- GROUP 会话 ID（非 `p_` 前缀）→ 仅写 Redis，不调用 PushToUser
- `expire` 参数为 `std::chrono::seconds(5)` 而非 10

- [ ] **Step 4: Commit（如有修正）**

```bash
git status
# 如有修正，commit
```

---

### Task 5: 编写功能测试（Go func test）

**Files:**
- Modify: `tests/func/presence_test.go`

- [ ] **Step 1: 添加 SendTyping PRIVATE 会话测试**

在 `tests/func/presence_test.go` 末尾新增：

```go
func TestSendTyping_PrivateChat_True(t *testing.T) {
	a, _, _ := fixture.RegisterAndLogin(t, HTTP)
	b, _, _ := fixture.RegisterAndLogin(t, HTTP)

	cid := "p_" + a.UserID + "_" + b.UserID
	if a.UserID > b.UserID {
		cid = "p_" + b.UserID + "_" + a.UserID
	}

	req := &presence.TypingReq{
		RequestId:      client.NewRequestID(),
		ConversationId: cid,
		IsTyping:       true,
	}
	rsp := &presence.TypingRsp{}
	err := a.DoAuth("/service/presence/send_typing", req, rsp)
	require.NoError(t, err)
	assert.True(t, rsp.Header.Success)
}

func TestSendTyping_PrivateChat_False(t *testing.T) {
	a, _, _ := fixture.RegisterAndLogin(t, HTTP)
	b, _, _ := fixture.RegisterAndLogin(t, HTTP)

	cid := "p_" + a.UserID + "_" + b.UserID
	if a.UserID > b.UserID {
		cid = "p_" + b.UserID + "_" + a.UserID
	}

	req := &presence.TypingReq{
		RequestId:      client.NewRequestID(),
		ConversationId: cid,
		IsTyping:       false,
	}
	rsp := &presence.TypingRsp{}
	err := a.DoAuth("/service/presence/send_typing", req, rsp)
	require.NoError(t, err)
	assert.True(t, rsp.Header.Success)
}

func TestSendTyping_GroupChat_NoOp(t *testing.T) {
	a, _, _ := fixture.RegisterAndLogin(t, HTTP)

	req := &presence.TypingReq{
		RequestId:      client.NewRequestID(),
		ConversationId: "g_some_group_id",
		IsTyping:       true,
	}
	rsp := &presence.TypingRsp{}
	err := a.DoAuth("/service/presence/send_typing", req, rsp)
	require.NoError(t, err)
	assert.True(t, rsp.Header.Success)
}
```

注意：PRIVATE 会话 ID 需要符合 `p_{lo}_{hi}` 格式（`a.UserID < b.UserID` 时 lo=a, hi=b）。

- [ ] **Step 2: 运行测试**

```bash
cd tests && go test -tags=func -run "TestSendTyping" -v -count=1 ./... 2>&1
```

Expected: 3 个测试 PASS。

- [ ] **Step 3: Commit**

```bash
git add tests/func/presence_test.go
git commit -m "test(presence): add SendTyping functional tests"
```
