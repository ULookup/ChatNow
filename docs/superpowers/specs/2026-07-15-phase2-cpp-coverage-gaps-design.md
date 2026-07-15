# Phase 2 C++ 覆盖缺口补齐设计

**目标：** 在保留 Task 22 C++ 测试删除的前提下，把 avatar URL、Gateway trace、MQ trace、media object key 与 magic sniff 的等价行为覆盖融入现有 Go 全栈测试框架。

**范围：** 仅修改 `tests/pkg/` 测试基础设施与 `tests/func/` 黑盒用例；不恢复 C++ 测试，不修改生产代码，不新增独立测试框架。

## 方案选择

采用“共享能力下沉到 `tests/pkg`，行为断言留在 `tests/func`”方案。

- 不采用“只修正文档映射”：无法补足 reviewer 发现的真实覆盖缺口。
- 不采用“恢复少量 C++ 单测”：违背统一 Go 黑盒测试架构与 Task 22 的删除目标。
- 不把所有逻辑堆进单个 scenario：会重复 HTTP、上传与 DB 查询代码，也无法沿用既有 fixture/verifier 约束。

## 框架集成

### `tests/pkg/client`

在 `HTTPClient` 保持现有 `Do` / `DoAuth` / `DoNoAuth` API 不变的前提下，增加 trace-aware 请求入口：

```go
func (c *HTTPClient) DoWithTrace(
    path string,
    req proto.Message,
    resp proto.Message,
    accessToken string,
    traceID string,
) (http.Header, error)
```

该方法设置 `X-Trace-Id`，返回响应 header。原有 `Do` 复用同一内部发送函数但丢弃 header，避免复制 HTTP/protobuf 逻辑。

### `tests/pkg/fixture`

增加按用途上传入口：

```go
func UploadFileForPurpose(
    t testing.TB,
    c *client.HTTPClient,
    content []byte,
    mime string,
    purpose media.MediaPurpose,
) string
```

现有 `UploadFile` 保持签名不变，内部以 `media.MediaPurpose_CHAT` 调用新入口。Fixture 只在不可继续时 `t.Fatal`，仍只返回 `fileID`，不承载行为断言。

### `tests/pkg/verify`

给 `DBVerifier` 增加只读媒体记录查询：

```go
type MediaFileRecord struct {
    Bucket    string
    ObjectKey string
    Status    int
}

func (v *DBVerifier) MediaFile(t testing.TB, fileID string) MediaFileRecord
```

测试函数负责对 object key、bucket、状态做断言；verifier 只负责稳定查询并在查询失败时终止当前测试。

## 用例设计

### FN-AM-06：Gateway Trace Header

文件：`tests/func/auth_middleware_test.go`

使用固定的 32 位小写十六进制 trace ID 调用真实 Gateway，断言响应 `X-Trace-Id` 与请求值完全一致。该用例替代已删除的 trace ID 工具单测，验证的是外部可观察的 Gateway 行为。

### FN-WS-08：MQ Trace Propagation

文件：`tests/func/ws_notify_test.go`

通过 `setupConv`/现有 fixture 建立会话并连接接收方 WS；发送消息时指定 trace ID，等待 `CHAT_MESSAGE_NOTIFY`，断言 `NotifyMessage.trace_id` 与发送请求相同。链路覆盖 Gateway metadata → transmite MQ header → push consumer → WS notify。

### FN-ID-08：Avatar Upload and URL

文件：`tests/func/identity_test.go`

使用 `UploadFileForPurpose(..., AVATAR)` 上传合法图片，再调用 `UpdateProfile(avatar_file_id)` 与 `GetProfile`。断言 URL 非空、使用公开媒体路径，并以 `/avatar/<fileID>` 结束。这是已删除 avatar URL 单测的外部行为替代。

代码层检查显示当前生产实现可能未包含 `/avatar/` 段；本 PR 不修改生产代码，因此该用例在真实环境运行时可能暴露现有生产缺陷。按用户要求本次不执行测试，也不声称其已通过。

### FN-MD-19：Object Key Layout

文件：`tests/func/media_test.go`

分别上传 CHAT 与 AVATAR 文件，通过 `DBVerifier.MediaFile` 验证：

- CHAT：private bucket，key 匹配 `chat/YYYY/MM/DD/<hash前2位>/<64位hash>`，并核对 hash 后缀。
- AVATAR：public bucket，key 精确匹配 `avatar/<64位hash>`。

该用例保留黑盒上传主路径，只用 DB 直查验证不可由公开 API 暴露的持久化 key。

### FN-MD-20：Magic Sniff Quarantine

文件：`tests/func/media_test.go`

上传声明为 `image/jpeg`、实际以 PE `MZ` magic 开头的内容并完成上传。使用 `require.Eventually` 轮询 `MediaFile.status`，最长 90 秒等待 cleanup worker 将其置为 `QUARANTINED`；随后调用下载 API，断言文件不可下载。

单次轮询不使用立即终止的断言。该用例复用 func stack 与统一 cleanup，不另建 reliability 目录；较长上限对应生产 worker 的 60 秒周期。

## 隔离与命名

- 继续使用 `tests/func/setup_test.go` 的 `TestMain`、`cleanup.CleanupAll`、`Cfg` 与 `HTTP`。
- 用例注释与函数命名使用现有方案：`FN-AM-06`、`FN-WS-08`、`FN-ID-08`、`FN-MD-19`、`FN-MD-20`。
- 所有上传内容、hash、request ID 与用户均为当前 run 唯一，避免 dedup 或残留数据造成串扰。
- 不新增 mock；所有行为经真实 HTTP/protobuf、WS、MinIO 上传和 MySQL 直查完成。

## 验收

- 新能力只存在于 `tests/pkg/{client,fixture,verify}`，业务断言只存在于 `tests/func`。
- 原有 public API 保持兼容，现有用例无需修改调用方式。
- 五个覆盖缺口均有明确 Go 用例 ID 与代码路径。
- Task 22 的 C++ 删除提交保持不变，不恢复任何 `.cc`。
- 本次只做编码、格式检查和静态 review；不运行测试、编译、vet 或 Docker，并明确记录该限制。

## 已知风险

- Avatar URL 用例可能揭示生产实现与既有 `/avatar/<fileID>` 合同不一致；不在测试 PR 中静默修改生产代码。
- Magic sniff 依赖 60 秒 cleanup 周期，真实执行耗时较长；用 90 秒最终一致窗口而非固定 sleep。
- 本地环境未就位，所有运行行为需后续在可用全栈或 CI 中验证。
