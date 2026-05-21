# API Documentation Design

## Goal

为前端 AI Agent 提供结构化、可解析的 OpenAPI 3.1 文档，覆盖 ChatNow 全部 59 个客户端 HTTP 端点。Agent 可直接解析生成前端调用代码。

## Design Decisions

- **Format**: OpenAPI 3.1 YAML，Agent 原生可解析
- **Split**: 按 domain 拆分为 7+1 个文件（每个 domain 独立 + 1 个公共类型文件），Agent 按需加载
- **Proto mapping**: 每个端点和 schema 附带 `x-protobuf` 扩展，精确指向 `.proto` 文件中的 service/rpc/message
- **Content type**: `application/x-protobuf`，前端直接消费 proto 生成 TS 代码
- **Auth**: 顶层 `security: [{bearerAuth: []}]` 为默认 JWT_REQUIRED，白名单端点覆盖 `security: []`

## File Layout

```
docs/api/
├── openapi-common.yaml       # ResponseHeader, UserInfo, PageRequest, PageResponse, ErrorCode
├── openapi-identity.yaml     # 9 端点
├── openapi-relationship.yaml # 9 端点
├── openapi-conversation.yaml # 18 端点
├── openapi-message.yaml      # 13 端点
├── openapi-transmite.yaml    # 1 端点
├── openapi-media.yaml        # 5 端点
└── openapi-presence.yaml     # 4 端点
```

## Endpoint Anatomy

每个端点包含：

```yaml
/service/{domain}/{method}:
  post:
    summary: <中文简述>
    description: <额外说明，如特殊行为>
    tags: [<Domain>]
    security: [] | [{bearerAuth: []}]
    x-protobuf:
      service: chatnow.<domain>.<ServiceName>
      rpc: <RpcMethodName>
      request: <RequestMessage>
      response: <ResponseMessage>
      file: proto/<domain>/<domain>_service.proto
    requestBody:
      required: true
      content:
        application/x-protobuf:
          schema:
            $ref: '#/components/schemas/<RequestMessage>'
    responses:
      '200':
        description: 成功
        content:
          application/x-protobuf:
            schema:
              allOf:
                - $ref: '../openapi-common.yaml#/components/schemas/ResponseHeader'
                - $ref: '#/components/schemas/<ResponseMessage>'
      '4xx':
        $ref: '#/components/responses/BusinessError'
```

## Shared Components (`openapi-common.yaml`)

- **ResponseHeader**: `{request_id, success, error_code, error_message}` — 所有响应都包裹这一层
- **UserInfo**: `{user_id, nickname, bio, phone, avatar_url}`
- **PageRequest**: cursor-based `{limit, cursor}`
- **PageResponse**: `{has_more, next_cursor, total_count}`
- **ErrorCode**: enum，按千位分段（1xxx=认证, 2xxx=关系, 3xxx=会话, 4xxx=消息, 5xxx=媒体, 6xxx=Presence, 7xxx=设备, 8xxx=限流, 9xxx=系统）

## x-protobuf Extension

| Field | Schema |
|-------|--------|
| `x-protobuf.service` | proto `service` 全限定名 |
| `x-protobuf.rpc` | proto `rpc` 方法名 |
| `x-protobuf.request` | 请求 message 名 |
| `x-protobuf.response` | 响应 message 名 |
| `x-protobuf.file` | proto 文件相对路径 |
| `x-protobuf.message` |（仅 schema 级别）proto message 名 |

## Auth Model

| 类型 | OpenAPI 表达 | 端点 |
|------|-------------|------|
| WHITELISTED | `security: []` | register, login, send_verify_code, refresh_token |
| JWT_REQUIRED | `security: [{bearerAuth: []}]` | 其余 55 个端点 |

JWT 通过 HTTP Header `Authorization: Bearer <access_token>` 传递。

## Special Timeouts

| 端点 | 超时 | 原因 |
|------|------|------|
| `/service/message/sync` | 10000ms | 长轮询消息同步 |
| `/service/transmite/send` | 1000ms | 消息发送快速通道 |
| 其余全部 | 3000ms | 默认 |

## Proto Oneof Handling

Proto `oneof` 在 OpenAPI 中表达为内嵌的 `oneOf`：

```yaml
credential:
  oneOf:
    - $ref: '#/components/schemas/UsernamePassword'
    - $ref: '#/components/schemas/PhoneVerifyCode'
```

## Endpoint Inventory

| Domain | File | Endpoints | WHITELISTED |
|--------|------|-----------|-------------|
| Identity | openapi-identity.yaml | 9 | 4 |
| Relationship | openapi-relationship.yaml | 9 | 0 |
| Conversation | openapi-conversation.yaml | 18 | 0 |
| Message | openapi-message.yaml | 13 | 0 |
| Transmite | openapi-transmite.yaml | 1 | 0 |
| Media | openapi-media.yaml | 5 | 0 |
| Presence | openapi-presence.yaml | 4 | 0 |
| **Total** | | **59** | **4** |

详见各 proto 文件及 `gateway/source/gateway_server.h` 的 `register_routes()`。
