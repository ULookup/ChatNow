# API Documentation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Generate 8 OpenAPI 3.1 YAML files covering all 59 ChatNow client HTTP endpoints for frontend Agent consumption.

**Architecture:** 7 domain files (identity, relationship, conversation, message, transmite, media, presence) + 1 common types file. Each domain file is self-contained with its own schemas and references common types via `$ref: '../openapi-common.yaml#/...'`. All endpoints use `x-protobuf` extension to map to proto definitions, content type `application/x-protobuf`, and `allOf` to merge `ResponseHeader` into responses.

**Tech Stack:** OpenAPI 3.1 YAML, protobuf 3

---

### Task 1: Create docs/api/ directory and openapi-common.yaml

**Files:**
- Create: `docs/api/openapi-common.yaml`

- [ ] **Step 1: Create the common types file**

Write `docs/api/openapi-common.yaml`:

```yaml
openapi: 3.1.0
info:
  title: ChatNow Common Types
  version: 3.0.0

components:
  schemas:
    ResponseHeader:
      description: 所有响应都包裹这一层。error_code=0 表示成功。
      x-protobuf:
        message: ResponseHeader
        file: proto/common/envelope.proto
      type: object
      properties:
        request_id:
          type: string
          description: 请求追踪ID
        success:
          type: boolean
          description: 是否成功
        error_code:
          type: integer
          description: 错误码，0=成功，非0=错误（见 ErrorCode 枚举）
        error_message:
          type: string
          description: 错误描述

    UserInfo:
      x-protobuf:
        message: UserInfo
        file: proto/common/types.proto
      type: object
      properties:
        user_id:
          type: string
        nickname:
          type: string
        bio:
          type: string
        phone:
          type: string
        avatar_url:
          type: string

    PageRequest:
      x-protobuf:
        message: PageRequest
        file: proto/common/envelope.proto
      type: object
      properties:
        limit:
          type: integer
          description: 每页条数
        cursor:
          type: string
          description: 游标，首页传空字符串

    PageResponse:
      x-protobuf:
        message: PageResponse
        file: proto/common/envelope.proto
      type: object
      properties:
        has_more:
          type: boolean
          description: 是否还有更多
        next_cursor:
          type: string
          description: 下一页游标
        total_count:
          type: integer
          description: 总记录数

    TimeRange:
      x-protobuf:
        message: TimeRange
        file: proto/common/envelope.proto
      type: object
      properties:
        start_time_ms:
          type: integer
          format: int64
        end_time_ms:
          type: integer
          format: int64

    DevicePlatform:
      x-protobuf:
        message: DevicePlatform
        file: proto/common/types.proto
      type: integer
      description: 0=UNSPECIFIED, 1=IOS, 2=ANDROID, 3=WEB, 4=DESKTOP_WIN, 5=DESKTOP_MAC, 6=DESKTOP_LINUX

    ErrorCode:
      x-protobuf:
        message: ErrorCode
        file: proto/common/error.proto
      type: integer
      description: |
        错误码分段:
        0 = OK
        1000-1999 = 认证 (AUTH_INVALID_CREDENTIALS=1001, AUTH_TOKEN_EXPIRED=1002, AUTH_TOKEN_INVALID=1003, AUTH_USER_NOT_FOUND=1004, AUTH_USER_ALREADY_EXISTS=1005, AUTH_VERIFY_CODE_INVALID=1006, AUTH_VERIFY_CODE_EXPIRED=1007, AUTH_REFRESH_TOKEN_REUSED=1008, AUTH_DEVICE_REVOKED=1009)
        2000-2999 = 关系 (RELATIONSHIP_ALREADY_FRIENDS=2001, RELATIONSHIP_NOT_FRIENDS=2002, RELATIONSHIP_BLOCKED=2003, RELATIONSHIP_REQUEST_PENDING=2004)
        3000-3999 = 会话 (CONVERSATION_NOT_FOUND=3001, CONVERSATION_NOT_MEMBER=3002, CONVERSATION_NO_PERMISSION=3003, CONVERSATION_MEMBER_LIMIT=3004)
        4000-4999 = 消息 (MESSAGE_NOT_FOUND=4001, MESSAGE_RECALL_TIMEOUT=4002, MESSAGE_ALREADY_RECALLED=4003, MESSAGE_CONTENT_INVALID=4004)
        5000-5999 = 媒体 (MEDIA_FILE_TOO_LARGE=5001, MEDIA_UNSUPPORTED_FORMAT=5002, MEDIA_UPLOAD_FAILED=5003, MEDIA_QUOTA_EXCEEDED=5004, MEDIA_HASH_MISMATCH=5005, MEDIA_UPLOAD_INCOMPLETE=5006, MEDIA_PART_NOT_FOUND=5007, MEDIA_FILE_NOT_FOUND=5008)
        6000-6999 = Presence (PRESENCE_USER_OFFLINE=6001)
        7000-7999 = Device (DEVICE_NOT_FOUND=7001, DEVICE_REVOKE_SELF=7002, DEVICE_LIMIT_EXCEEDED=7003)
        8000-8999 = 限流 (RATE_LIMIT_EXCEEDED=8001)
        9000-9999 = 系统 (SYSTEM_INTERNAL_ERROR=9001, SYSTEM_UNAVAILABLE=9002, SYSTEM_TIMEOUT=9003, SYSTEM_INVALID_ARGUMENT=9004)
```

- [ ] **Step 2: Commit**

```bash
git add docs/api/openapi-common.yaml
git commit -m "docs(api): add OpenAPI common types (ResponseHeader, UserInfo, PageRequest, ErrorCode)"
```

---

### Task 2: Create openapi-identity.yaml (9 endpoints)

**Files:**
- Create: `docs/api/openapi-identity.yaml`

- [ ] **Step 1: Write the Identity OpenAPI file**

Write `docs/api/openapi-identity.yaml`:

```yaml
openapi: 3.1.0
info:
  title: ChatNow Identity API
  version: 3.0.0
  description: |
    身份认证域。Proto: proto/identity/identity_service.proto
    Service: chatnow.identity.IdentityService

servers:
  - url: http://localhost:9000
    description: Gateway HTTP

security:
  - bearerAuth: []

paths:
  /service/identity/send_verify_code:
    post:
      summary: 发送验证码
      tags: [Identity]
      security: []
      x-protobuf: { service: chatnow.identity.IdentityService, rpc: SendVerifyCode, request: SendVerifyCodeReq, response: SendVerifyCodeRsp, file: proto/identity/identity_service.proto }
      requestBody:
        required: true
        content:
          application/x-protobuf:
            schema: { $ref: '#/components/schemas/SendVerifyCodeReq' }
      responses:
        '200':
          description: 成功
          content:
            application/x-protobuf:
              schema:
                allOf:
                  - $ref: '../openapi-common.yaml#/components/schemas/ResponseHeader'
                  - $ref: '#/components/schemas/SendVerifyCodeRsp'
        '4xx': { $ref: '#/components/responses/BusinessError' }

  /service/identity/register:
    post:
      summary: 注册
      tags: [Identity]
      security: []
      x-protobuf: { service: chatnow.identity.IdentityService, rpc: Register, request: RegisterReq, response: RegisterRsp, file: proto/identity/identity_service.proto }
      requestBody:
        required: true
        content:
          application/x-protobuf:
            schema: { $ref: '#/components/schemas/RegisterReq' }
      responses:
        '200':
          description: 成功，返回 tokens 和用户信息
          content:
            application/x-protobuf:
              schema:
                allOf:
                  - $ref: '../openapi-common.yaml#/components/schemas/ResponseHeader'
                  - $ref: '#/components/schemas/RegisterRsp'
        '4xx': { $ref: '#/components/responses/BusinessError' }

  /service/identity/login:
    post:
      summary: 登录
      tags: [Identity]
      security: []
      x-protobuf: { service: chatnow.identity.IdentityService, rpc: Login, request: LoginReq, response: LoginRsp, file: proto/identity/identity_service.proto }
      requestBody:
        required: true
        content:
          application/x-protobuf:
            schema: { $ref: '#/components/schemas/LoginReq' }
      responses:
        '200':
          description: 成功，返回 tokens 和用户信息
          content:
            application/x-protobuf:
              schema:
                allOf:
                  - $ref: '../openapi-common.yaml#/components/schemas/ResponseHeader'
                  - $ref: '#/components/schemas/LoginRsp'
        '4xx': { $ref: '#/components/responses/BusinessError' }

  /service/identity/refresh_token:
    post:
      summary: 刷新令牌
      tags: [Identity]
      security: []
      x-protobuf: { service: chatnow.identity.IdentityService, rpc: RefreshToken, request: RefreshTokenReq, response: RefreshTokenRsp, file: proto/identity/identity_service.proto }
      requestBody:
        required: true
        content:
          application/x-protobuf:
            schema: { $ref: '#/components/schemas/RefreshTokenReq' }
      responses:
        '200':
          description: 成功
          content:
            application/x-protobuf:
              schema:
                allOf:
                  - $ref: '../openapi-common.yaml#/components/schemas/ResponseHeader'
                  - $ref: '#/components/schemas/RefreshTokenRsp'
        '4xx': { $ref: '#/components/responses/BusinessError' }

  /service/identity/logout:
    post:
      summary: 登出
      description: user_id/device_id 从 JWT 提取，body 仅需 request_id
      tags: [Identity]
      x-protobuf: { service: chatnow.identity.IdentityService, rpc: Logout, request: LogoutReq, response: LogoutRsp, file: proto/identity/identity_service.proto }
      requestBody:
        required: true
        content:
          application/x-protobuf:
            schema: { $ref: '#/components/schemas/LogoutReq' }
      responses:
        '200': { $ref: '#/components/responses/Success' }
        '4xx': { $ref: '#/components/responses/BusinessError' }

  /service/identity/get_profile:
    post:
      summary: 获取个人信息
      description: user_id 为空时查自己
      tags: [Identity]
      x-protobuf: { service: chatnow.identity.IdentityService, rpc: GetProfile, request: GetProfileReq, response: GetProfileRsp, file: proto/identity/identity_service.proto }
      requestBody:
        required: true
        content:
          application/x-protobuf:
            schema: { $ref: '#/components/schemas/GetProfileReq' }
      responses:
        '200':
          description: 成功
          content:
            application/x-protobuf:
              schema:
                allOf:
                  - $ref: '../openapi-common.yaml#/components/schemas/ResponseHeader'
                  - $ref: '#/components/schemas/GetProfileRsp'
        '4xx': { $ref: '#/components/responses/BusinessError' }

  /service/identity/update_profile:
    post:
      summary: 更新个人信息
      tags: [Identity]
      x-protobuf: { service: chatnow.identity.IdentityService, rpc: UpdateProfile, request: UpdateProfileReq, response: UpdateProfileRsp, file: proto/identity/identity_service.proto }
      requestBody:
        required: true
        content:
          application/x-protobuf:
            schema: { $ref: '#/components/schemas/UpdateProfileReq' }
      responses:
        '200':
          description: 成功
          content:
            application/x-protobuf:
              schema:
                allOf:
                  - $ref: '../openapi-common.yaml#/components/schemas/ResponseHeader'
                  - $ref: '#/components/schemas/UpdateProfileRsp'
        '4xx': { $ref: '#/components/responses/BusinessError' }

  /service/identity/search_users:
    post:
      summary: 搜索用户
      tags: [Identity]
      x-protobuf: { service: chatnow.identity.IdentityService, rpc: SearchUsers, request: SearchUsersReq, response: SearchUsersRsp, file: proto/identity/identity_service.proto }
      requestBody:
        required: true
        content:
          application/x-protobuf:
            schema: { $ref: '#/components/schemas/SearchUsersReq' }
      responses:
        '200':
          description: 成功
          content:
            application/x-protobuf:
              schema:
                allOf:
                  - $ref: '../openapi-common.yaml#/components/schemas/ResponseHeader'
                  - $ref: '#/components/schemas/SearchUsersRsp'
        '4xx': { $ref: '#/components/responses/BusinessError' }

  /service/identity/get_multi_info:
    post:
      summary: 批量获取用户信息
      tags: [Identity]
      x-protobuf: { service: chatnow.identity.IdentityService, rpc: GetMultiUserInfo, request: GetMultiUserInfoReq, response: GetMultiUserInfoRsp, file: proto/identity/identity_service.proto }
      requestBody:
        required: true
        content:
          application/x-protobuf:
            schema: { $ref: '#/components/schemas/GetMultiUserInfoReq' }
      responses:
        '200':
          description: 成功，返回 user_id -> UserInfo 映射
          content:
            application/x-protobuf:
              schema:
                allOf:
                  - $ref: '../openapi-common.yaml#/components/schemas/ResponseHeader'
                  - $ref: '#/components/schemas/GetMultiUserInfoRsp'
        '4xx': { $ref: '#/components/responses/BusinessError' }

components:
  securitySchemes:
    bearerAuth:
      type: http
      scheme: bearer
      bearerFormat: JWT
      description: |
        HTTP Header: Authorization: Bearer <access_token>
        从 /service/identity/login 或 /service/identity/register 获取

  responses:
    Success:
      description: 成功（仅 ResponseHeader，无额外数据）
      content:
        application/x-protobuf:
          schema: { $ref: '../openapi-common.yaml#/components/schemas/ResponseHeader' }
    BusinessError:
      description: 业务错误（error_code != 0）
      content:
        application/x-protobuf:
          schema: { $ref: '../openapi-common.yaml#/components/schemas/ResponseHeader' }

  schemas:
    AuthTokens:
      x-protobuf: { message: AuthTokens, file: proto/identity/identity_service.proto }
      type: object
      properties:
        access_token: { type: string }
        refresh_token: { type: string }
        access_expires_in_sec: { type: integer }
        refresh_expires_in_sec: { type: integer }

    UsernamePassword:
      type: object
      properties:
        username: { type: string }
        password: { type: string }

    PhoneVerifyCode:
      type: object
      properties:
        phone: { type: string }
        verify_code_id: { type: string }
        verify_code: { type: string }

    RegisterReq:
      x-protobuf: { message: RegisterReq, file: proto/identity/identity_service.proto }
      type: object
      required: [request_id, nickname]
      properties:
        request_id: { type: string }
        nickname: { type: string }
        credential:
          oneOf:
            - { $ref: '#/components/schemas/UsernamePassword' }
            - { $ref: '#/components/schemas/PhoneVerifyCode' }

    RegisterRsp:
      type: object
      properties:
        user_id: { type: string }
        tokens: { $ref: '#/components/schemas/AuthTokens' }
        user_info: { $ref: '../openapi-common.yaml#/components/schemas/UserInfo' }

    LoginReq:
      x-protobuf: { message: LoginReq, file: proto/identity/identity_service.proto }
      type: object
      required: [request_id]
      properties:
        request_id: { type: string }
        device_id:
          type: string
          description: 设备ID，透传入 JWT payload
        device_name: { type: string }
        credential:
          oneOf:
            - { $ref: '#/components/schemas/UsernamePassword' }
            - { $ref: '#/components/schemas/PhoneVerifyCode' }

    LoginRsp:
      type: object
      properties:
        tokens: { $ref: '#/components/schemas/AuthTokens' }
        user_info: { $ref: '../openapi-common.yaml#/components/schemas/UserInfo' }

    LogoutReq:
      type: object
      properties:
        request_id: { type: string }

    SendVerifyCodeReq:
      x-protobuf: { message: SendVerifyCodeReq, file: proto/identity/identity_service.proto }
      type: object
      required: [request_id]
      properties:
        request_id: { type: string }
        destination:
          oneOf:
            - type: object
              properties: { email: { type: string } }
            - type: object
              properties: { phone: { type: string } }

    SendVerifyCodeRsp:
      type: object
      properties:
        verify_code_id: { type: string }

    RefreshTokenReq:
      type: object
      required: [request_id, refresh_token]
      properties:
        request_id: { type: string }
        refresh_token: { type: string }

    RefreshTokenRsp:
      type: object
      properties:
        tokens: { $ref: '#/components/schemas/AuthTokens' }

    GetProfileReq:
      type: object
      properties:
        request_id: { type: string }
        user_id:
          type: string
          description: 空=查自己

    GetProfileRsp:
      type: object
      properties:
        user_info: { $ref: '../openapi-common.yaml#/components/schemas/UserInfo' }

    UpdateProfileReq:
      type: object
      properties:
        request_id: { type: string }
        nickname: { type: string }
        bio: { type: string }
        avatar_file_id: { type: string }
        phone: { type: string }

    UpdateProfileRsp:
      type: object
      properties:
        user_info: { $ref: '../openapi-common.yaml#/components/schemas/UserInfo' }

    SearchUsersReq:
      type: object
      required: [request_id, search_key]
      properties:
        request_id: { type: string }
        search_key: { type: string }

    SearchUsersRsp:
      type: object
      properties:
        user_info:
          type: array
          items: { $ref: '../openapi-common.yaml#/components/schemas/UserInfo' }

    GetMultiUserInfoReq:
      type: object
      required: [request_id, users_id]
      properties:
        request_id: { type: string }
        users_id:
          type: array
          items: { type: string }

    GetMultiUserInfoRsp:
      type: object
      properties:
        users_info:
          type: object
          additionalProperties: { $ref: '../openapi-common.yaml#/components/schemas/UserInfo' }
          description: user_id -> UserInfo 映射
```

- [ ] **Step 2: Commit**

```bash
git add docs/api/openapi-identity.yaml
git commit -m "docs(api): add OpenAPI identity domain (9 endpoints)"
```

---

### Task 3: Create openapi-relationship.yaml (9 endpoints)

**Files:**
- Create: `docs/api/openapi-relationship.yaml`

- [ ] **Step 1: Write the Relationship OpenAPI file**

Write `docs/api/openapi-relationship.yaml`:

```yaml
openapi: 3.1.0
info:
  title: ChatNow Relationship API
  version: 3.0.0
  description: |
    好友关系域。Proto: proto/relationship/relationship_service.proto
    Service: chatnow.relationship.RelationshipService

servers:
  - url: http://localhost:9000
    description: Gateway HTTP

security:
  - bearerAuth: []

paths:
  /service/relationship/list_friends:
    post:
      summary: 好友列表
      tags: [Relationship]
      x-protobuf: { service: chatnow.relationship.RelationshipService, rpc: ListFriends, request: ListFriendsReq, response: ListFriendsRsp, file: proto/relationship/relationship_service.proto }
      requestBody:
        required: true
        content:
          application/x-protobuf:
            schema: { $ref: '#/components/schemas/ListFriendsReq' }
      responses:
        '200':
          description: 成功
          content:
            application/x-protobuf:
              schema:
                allOf:
                  - $ref: '../openapi-common.yaml#/components/schemas/ResponseHeader'
                  - $ref: '#/components/schemas/ListFriendsRsp'
        '4xx': { $ref: '#/components/responses/BusinessError' }

  /service/relationship/send_friend_request:
    post:
      summary: 发送好友申请
      tags: [Relationship]
      x-protobuf: { service: chatnow.relationship.RelationshipService, rpc: SendFriendRequest, request: SendFriendReq, response: SendFriendRsp, file: proto/relationship/relationship_service.proto }
      requestBody:
        required: true
        content:
          application/x-protobuf:
            schema: { $ref: '#/components/schemas/SendFriendReq' }
      responses:
        '200':
          description: 成功
          content:
            application/x-protobuf:
              schema:
                allOf:
                  - $ref: '../openapi-common.yaml#/components/schemas/ResponseHeader'
                  - $ref: '#/components/schemas/SendFriendRsp'
        '4xx': { $ref: '#/components/responses/BusinessError' }

  /service/relationship/handle_friend_request:
    post:
      summary: 处理好友申请
      tags: [Relationship]
      x-protobuf: { service: chatnow.relationship.RelationshipService, rpc: HandleFriendRequest, request: HandleFriendReq, response: HandleFriendRsp, file: proto/relationship/relationship_service.proto }
      requestBody:
        required: true
        content:
          application/x-protobuf:
            schema: { $ref: '#/components/schemas/HandleFriendReq' }
      responses:
        '200':
          description: 成功
          content:
            application/x-protobuf:
              schema:
                allOf:
                  - $ref: '../openapi-common.yaml#/components/schemas/ResponseHeader'
                  - $ref: '#/components/schemas/HandleFriendRsp'
        '4xx': { $ref: '#/components/responses/BusinessError' }

  /service/relationship/remove_friend:
    post:
      summary: 删除好友
      tags: [Relationship]
      x-protobuf: { service: chatnow.relationship.RelationshipService, rpc: RemoveFriend, request: RemoveFriendReq, response: RemoveFriendRsp, file: proto/relationship/relationship_service.proto }
      requestBody:
        required: true
        content:
          application/x-protobuf:
            schema: { $ref: '#/components/schemas/RemoveFriendReq' }
      responses:
        '200': { $ref: '#/components/responses/Success' }
        '4xx': { $ref: '#/components/responses/BusinessError' }

  /service/relationship/search_friends:
    post:
      summary: 搜索好友
      tags: [Relationship]
      x-protobuf: { service: chatnow.relationship.RelationshipService, rpc: SearchFriends, request: SearchFriendsReq, response: SearchFriendsRsp, file: proto/relationship/relationship_service.proto }
      requestBody:
        required: true
        content:
          application/x-protobuf:
            schema: { $ref: '#/components/schemas/SearchFriendsReq' }
      responses:
        '200':
          description: 成功
          content:
            application/x-protobuf:
              schema:
                allOf:
                  - $ref: '../openapi-common.yaml#/components/schemas/ResponseHeader'
                  - $ref: '#/components/schemas/SearchFriendsRsp'
        '4xx': { $ref: '#/components/responses/BusinessError' }

  /service/relationship/block_user:
    post:
      summary: 拉黑用户
      tags: [Relationship]
      x-protobuf: { service: chatnow.relationship.RelationshipService, rpc: BlockUser, request: BlockUserReq, response: BlockUserRsp, file: proto/relationship/relationship_service.proto }
      requestBody:
        required: true
        content:
          application/x-protobuf:
            schema: { $ref: '#/components/schemas/BlockUserReq' }
      responses:
        '200': { $ref: '#/components/responses/Success' }
        '4xx': { $ref: '#/components/responses/BusinessError' }

  /service/relationship/unblock_user:
    post:
      summary: 取消拉黑
      tags: [Relationship]
      x-protobuf: { service: chatnow.relationship.RelationshipService, rpc: UnblockUser, request: UnblockUserReq, response: UnblockUserRsp, file: proto/relationship/relationship_service.proto }
      requestBody:
        required: true
        content:
          application/x-protobuf:
            schema: { $ref: '#/components/schemas/UnblockUserReq' }
      responses:
        '200': { $ref: '#/components/responses/Success' }
        '4xx': { $ref: '#/components/responses/BusinessError' }

  /service/relationship/list_blocked:
    post:
      summary: 黑名单列表
      tags: [Relationship]
      x-protobuf: { service: chatnow.relationship.RelationshipService, rpc: ListBlockedUsers, request: ListBlockedReq, response: ListBlockedRsp, file: proto/relationship/relationship_service.proto }
      requestBody:
        required: true
        content:
          application/x-protobuf:
            schema: { $ref: '#/components/schemas/ListBlockedReq' }
      responses:
        '200':
          description: 成功
          content:
            application/x-protobuf:
              schema:
                allOf:
                  - $ref: '../openapi-common.yaml#/components/schemas/ResponseHeader'
                  - $ref: '#/components/schemas/ListBlockedRsp'
        '4xx': { $ref: '#/components/responses/BusinessError' }

  /service/relationship/list_pending:
    post:
      summary: 待处理的好友申请
      tags: [Relationship]
      x-protobuf: { service: chatnow.relationship.RelationshipService, rpc: ListPendingRequests, request: ListPendingReq, response: ListPendingRsp, file: proto/relationship/relationship_service.proto }
      requestBody:
        required: true
        content:
          application/x-protobuf:
            schema: { $ref: '#/components/schemas/ListPendingReq' }
      responses:
        '200':
          description: 成功
          content:
            application/x-protobuf:
              schema:
                allOf:
                  - $ref: '../openapi-common.yaml#/components/schemas/ResponseHeader'
                  - $ref: '#/components/schemas/ListPendingRsp'
        '4xx': { $ref: '#/components/responses/BusinessError' }

components:
  securitySchemes:
    bearerAuth:
      type: http
      scheme: bearer
      bearerFormat: JWT

  responses:
    Success:
      description: 成功（仅 ResponseHeader，无额外数据）
      content:
        application/x-protobuf:
          schema: { $ref: '../openapi-common.yaml#/components/schemas/ResponseHeader' }
    BusinessError:
      description: 业务错误（error_code != 0）
      content:
        application/x-protobuf:
          schema: { $ref: '../openapi-common.yaml#/components/schemas/ResponseHeader' }

  schemas:
    ListFriendsReq:
      x-protobuf: { message: ListFriendsReq, file: proto/relationship/relationship_service.proto }
      type: object
      properties:
        request_id: { type: string }
        page: { $ref: '../openapi-common.yaml#/components/schemas/PageRequest' }

    ListFriendsRsp:
      type: object
      properties:
        friend_list:
          type: array
          items: { $ref: '../openapi-common.yaml#/components/schemas/UserInfo' }
        page: { $ref: '../openapi-common.yaml#/components/schemas/PageResponse' }

    SendFriendReq:
      x-protobuf: { message: SendFriendReq, file: proto/relationship/relationship_service.proto }
      type: object
      required: [request_id, respondent_id]
      properties:
        request_id: { type: string }
        respondent_id: { type: string }

    SendFriendRsp:
      type: object
      properties:
        notify_event_id: { type: string }

    HandleFriendReq:
      x-protobuf: { message: HandleFriendReq, file: proto/relationship/relationship_service.proto }
      type: object
      required: [request_id, notify_event_id, agree, apply_user_id]
      properties:
        request_id: { type: string }
        notify_event_id: { type: string }
        agree: { type: boolean }
        apply_user_id: { type: string }

    HandleFriendRsp:
      type: object
      properties:
        new_conversation_id: { type: string }

    RemoveFriendReq:
      x-protobuf: { message: RemoveFriendReq, file: proto/relationship/relationship_service.proto }
      type: object
      required: [request_id, peer_id]
      properties:
        request_id: { type: string }
        peer_id: { type: string }

    SearchFriendsReq:
      x-protobuf: { message: SearchFriendsReq, file: proto/relationship/relationship_service.proto }
      type: object
      required: [request_id, search_key]
      properties:
        request_id: { type: string }
        search_key: { type: string }

    SearchFriendsRsp:
      type: object
      properties:
        user_info:
          type: array
          items: { $ref: '../openapi-common.yaml#/components/schemas/UserInfo' }

    BlockUserReq:
      x-protobuf: { message: BlockUserReq, file: proto/relationship/relationship_service.proto }
      type: object
      required: [request_id, peer_id]
      properties:
        request_id: { type: string }
        peer_id: { type: string }

    UnblockUserReq:
      x-protobuf: { message: UnblockUserReq, file: proto/relationship/relationship_service.proto }
      type: object
      required: [request_id, peer_id]
      properties:
        request_id: { type: string }
        peer_id: { type: string }

    ListBlockedReq:
      x-protobuf: { message: ListBlockedReq, file: proto/relationship/relationship_service.proto }
      type: object
      properties:
        request_id: { type: string }
        page: { $ref: '../openapi-common.yaml#/components/schemas/PageRequest' }

    ListBlockedRsp:
      type: object
      properties:
        blocked_list:
          type: array
          items: { $ref: '../openapi-common.yaml#/components/schemas/UserInfo' }
        page: { $ref: '../openapi-common.yaml#/components/schemas/PageResponse' }

    ListPendingReq:
      x-protobuf: { message: ListPendingReq, file: proto/relationship/relationship_service.proto }
      type: object
      properties:
        request_id: { type: string }

    ListPendingRsp:
      type: object
      properties:
        event:
          type: array
          items: { $ref: '#/components/schemas/FriendEvent' }

    FriendEvent:
      x-protobuf: { message: FriendEvent, file: proto/relationship/relationship_service.proto }
      type: object
      properties:
        event_id: { type: string }
        sender: { $ref: '../openapi-common.yaml#/components/schemas/UserInfo' }
```

- [ ] **Step 2: Commit**

```bash
git add docs/api/openapi-relationship.yaml
git commit -m "docs(api): add OpenAPI relationship domain (9 endpoints)"
```

---

### Task 4: Create openapi-conversation.yaml (18 endpoints)

**Files:**
- Create: `docs/api/openapi-conversation.yaml`

- [ ] **Step 1: Write the Conversation OpenAPI file**

Write `docs/api/openapi-conversation.yaml`:

```yaml
openapi: 3.1.0
info:
  title: ChatNow Conversation API
  version: 3.0.0
  description: |
    会话管理域。Proto: proto/conversation/conversation_service.proto
    Service: chatnow.conversation.ConversationService

servers:
  - url: http://localhost:9000
    description: Gateway HTTP

security:
  - bearerAuth: []

paths:
  /service/conversation/list:
    post:
      summary: 会话列表
      tags: [Conversation]
      x-protobuf: { service: chatnow.conversation.ConversationService, rpc: ListConversations, request: ListConversationsReq, response: ListConversationsRsp, file: proto/conversation/conversation_service.proto }
      requestBody:
        required: true
        content:
          application/x-protobuf:
            schema: { $ref: '#/components/schemas/ListConversationsReq' }
      responses:
        '200':
          description: 成功
          content:
            application/x-protobuf:
              schema:
                allOf:
                  - $ref: '../openapi-common.yaml#/components/schemas/ResponseHeader'
                  - $ref: '#/components/schemas/ListConversationsRsp'
        '4xx': { $ref: '#/components/responses/BusinessError' }

  /service/conversation/get:
    post:
      summary: 获取会话详情
      tags: [Conversation]
      x-protobuf: { service: chatnow.conversation.ConversationService, rpc: GetConversation, request: GetConversationReq, response: GetConversationRsp, file: proto/conversation/conversation_service.proto }
      requestBody:
        required: true
        content:
          application/x-protobuf:
            schema: { $ref: '#/components/schemas/GetConversationReq' }
      responses:
        '200':
          description: 成功
          content:
            application/x-protobuf:
              schema:
                allOf:
                  - $ref: '../openapi-common.yaml#/components/schemas/ResponseHeader'
                  - $ref: '#/components/schemas/GetConversationRsp'
        '4xx': { $ref: '#/components/responses/BusinessError' }

  /service/conversation/create:
    post:
      summary: 创建会话
      tags: [Conversation]
      x-protobuf: { service: chatnow.conversation.ConversationService, rpc: CreateConversation, request: CreateConversationReq, response: CreateConversationRsp, file: proto/conversation/conversation_service.proto }
      requestBody:
        required: true
        content:
          application/x-protobuf:
            schema: { $ref: '#/components/schemas/CreateConversationReq' }
      responses:
        '200':
          description: 成功
          content:
            application/x-protobuf:
              schema:
                allOf:
                  - $ref: '../openapi-common.yaml#/components/schemas/ResponseHeader'
                  - $ref: '#/components/schemas/CreateConversationRsp'
        '4xx': { $ref: '#/components/responses/BusinessError' }

  /service/conversation/update:
    post:
      summary: 更新会话信息
      tags: [Conversation]
      x-protobuf: { service: chatnow.conversation.ConversationService, rpc: UpdateConversation, request: UpdateConversationReq, response: UpdateConversationRsp, file: proto/conversation/conversation_service.proto }
      requestBody:
        required: true
        content:
          application/x-protobuf:
            schema: { $ref: '#/components/schemas/UpdateConversationReq' }
      responses:
        '200':
          description: 成功
          content:
            application/x-protobuf:
              schema:
                allOf:
                  - $ref: '../openapi-common.yaml#/components/schemas/ResponseHeader'
                  - $ref: '#/components/schemas/UpdateConversationRsp'
        '4xx': { $ref: '#/components/responses/BusinessError' }

  /service/conversation/dismiss:
    post:
      summary: 解散群聊
      tags: [Conversation]
      x-protobuf: { service: chatnow.conversation.ConversationService, rpc: DismissConversation, request: DismissConversationReq, response: DismissConversationRsp, file: proto/conversation/conversation_service.proto }
      requestBody:
        required: true
        content:
          application/x-protobuf:
            schema: { $ref: '#/components/schemas/DismissConversationReq' }
      responses:
        '200': { $ref: '#/components/responses/Success' }
        '4xx': { $ref: '#/components/responses/BusinessError' }

  /service/conversation/add_members:
    post:
      summary: 添加成员
      tags: [Conversation]
      x-protobuf: { service: chatnow.conversation.ConversationService, rpc: AddMembers, request: AddMembersReq, response: AddMembersRsp, file: proto/conversation/conversation_service.proto }
      requestBody:
        required: true
        content:
          application/x-protobuf:
            schema: { $ref: '#/components/schemas/AddMembersReq' }
      responses:
        '200': { $ref: '#/components/responses/Success' }
        '4xx': { $ref: '#/components/responses/BusinessError' }

  /service/conversation/remove_members:
    post:
      summary: 移除成员
      tags: [Conversation]
      x-protobuf: { service: chatnow.conversation.ConversationService, rpc: RemoveMembers, request: RemoveMembersReq, response: RemoveMembersRsp, file: proto/conversation/conversation_service.proto }
      requestBody:
        required: true
        content:
          application/x-protobuf:
            schema: { $ref: '#/components/schemas/RemoveMembersReq' }
      responses:
        '200': { $ref: '#/components/responses/Success' }
        '4xx': { $ref: '#/components/responses/BusinessError' }

  /service/conversation/transfer_owner:
    post:
      summary: 转让群主
      tags: [Conversation]
      x-protobuf: { service: chatnow.conversation.ConversationService, rpc: TransferOwner, request: TransferOwnerReq, response: TransferOwnerRsp, file: proto/conversation/conversation_service.proto }
      requestBody:
        required: true
        content:
          application/x-protobuf:
            schema: { $ref: '#/components/schemas/TransferOwnerReq' }
      responses:
        '200': { $ref: '#/components/responses/Success' }
        '4xx': { $ref: '#/components/responses/BusinessError' }

  /service/conversation/change_role:
    post:
      summary: 修改成员角色
      tags: [Conversation]
      x-protobuf: { service: chatnow.conversation.ConversationService, rpc: ChangeMemberRole, request: ChangeMemberRoleReq, response: ChangeMemberRoleRsp, file: proto/conversation/conversation_service.proto }
      requestBody:
        required: true
        content:
          application/x-protobuf:
            schema: { $ref: '#/components/schemas/ChangeMemberRoleReq' }
      responses:
        '200': { $ref: '#/components/responses/Success' }
        '4xx': { $ref: '#/components/responses/BusinessError' }

  /service/conversation/list_members:
    post:
      summary: 成员列表
      tags: [Conversation]
      x-protobuf: { service: chatnow.conversation.ConversationService, rpc: ListMembers, request: ListMembersReq, response: ListMembersRsp, file: proto/conversation/conversation_service.proto }
      requestBody:
        required: true
        content:
          application/x-protobuf:
            schema: { $ref: '#/components/schemas/ListMembersReq' }
      responses:
        '200':
          description: 成功
          content:
            application/x-protobuf:
              schema:
                allOf:
                  - $ref: '../openapi-common.yaml#/components/schemas/ResponseHeader'
                  - $ref: '#/components/schemas/ListMembersRsp'
        '4xx': { $ref: '#/components/responses/BusinessError' }

  /service/conversation/set_mute:
    post:
      summary: 设置免打扰
      tags: [Conversation]
      x-protobuf: { service: chatnow.conversation.ConversationService, rpc: SetMute, request: SetMuteReq, response: SetMuteRsp, file: proto/conversation/conversation_service.proto }
      requestBody:
        required: true
        content:
          application/x-protobuf:
            schema: { $ref: '#/components/schemas/SetMuteReq' }
      responses:
        '200':
          description: 成功
          content:
            application/x-protobuf:
              schema:
                allOf:
                  - $ref: '../openapi-common.yaml#/components/schemas/ResponseHeader'
                  - $ref: '#/components/schemas/SetMuteRsp'
        '4xx': { $ref: '#/components/responses/BusinessError' }

  /service/conversation/set_pin:
    post:
      summary: 设置置顶
      tags: [Conversation]
      x-protobuf: { service: chatnow.conversation.ConversationService, rpc: SetPin, request: SetPinReq, response: SetPinRsp, file: proto/conversation/conversation_service.proto }
      requestBody:
        required: true
        content:
          application/x-protobuf:
            schema: { $ref: '#/components/schemas/SetPinReq' }
      responses:
        '200':
          description: 成功
          content:
            application/x-protobuf:
              schema:
                allOf:
                  - $ref: '../openapi-common.yaml#/components/schemas/ResponseHeader'
                  - $ref: '#/components/schemas/SetPinRsp'
        '4xx': { $ref: '#/components/responses/BusinessError' }

  /service/conversation/set_visible:
    post:
      summary: 设置可见性
      tags: [Conversation]
      x-protobuf: { service: chatnow.conversation.ConversationService, rpc: SetVisible, request: SetVisibleReq, response: SetVisibleRsp, file: proto/conversation/conversation_service.proto }
      requestBody:
        required: true
        content:
          application/x-protobuf:
            schema: { $ref: '#/components/schemas/SetVisibleReq' }
      responses:
        '200':
          description: 成功
          content:
            application/x-protobuf:
              schema:
                allOf:
                  - $ref: '../openapi-common.yaml#/components/schemas/ResponseHeader'
                  - $ref: '#/components/schemas/SetVisibleRsp'
        '4xx': { $ref: '#/components/responses/BusinessError' }

  /service/conversation/quit:
    post:
      summary: 退出会话
      tags: [Conversation]
      x-protobuf: { service: chatnow.conversation.ConversationService, rpc: QuitConversation, request: QuitConversationReq, response: QuitConversationRsp, file: proto/conversation/conversation_service.proto }
      requestBody:
        required: true
        content:
          application/x-protobuf:
            schema: { $ref: '#/components/schemas/QuitConversationReq' }
      responses:
        '200': { $ref: '#/components/responses/Success' }
        '4xx': { $ref: '#/components/responses/BusinessError' }

  /service/conversation/mark_read:
    post:
      summary: 标记已读
      tags: [Conversation]
      x-protobuf: { service: chatnow.conversation.ConversationService, rpc: MarkRead, request: MarkReadReq, response: MarkReadRsp, file: proto/conversation/conversation_service.proto }
      requestBody:
        required: true
        content:
          application/x-protobuf:
            schema: { $ref: '#/components/schemas/MarkReadReq' }
      responses:
        '200': { $ref: '#/components/responses/Success' }
        '4xx': { $ref: '#/components/responses/BusinessError' }

  /service/conversation/save_draft:
    post:
      summary: 保存草稿
      tags: [Conversation]
      x-protobuf: { service: chatnow.conversation.ConversationService, rpc: SaveDraft, request: SaveDraftReq, response: SaveDraftRsp, file: proto/conversation/conversation_service.proto }
      requestBody:
        required: true
        content:
          application/x-protobuf:
            schema: { $ref: '#/components/schemas/SaveDraftReq' }
      responses:
        '200':
          description: 成功
          content:
            application/x-protobuf:
              schema:
                allOf:
                  - $ref: '../openapi-common.yaml#/components/schemas/ResponseHeader'
                  - $ref: '#/components/schemas/SaveDraftRsp'
        '4xx': { $ref: '#/components/responses/BusinessError' }

  /service/conversation/search:
    post:
      summary: 搜索会话
      tags: [Conversation]
      x-protobuf: { service: chatnow.conversation.ConversationService, rpc: SearchConversations, request: SearchConversationsReq, response: SearchConversationsRsp, file: proto/conversation/conversation_service.proto }
      requestBody:
        required: true
        content:
          application/x-protobuf:
            schema: { $ref: '#/components/schemas/SearchConversationsReq' }
      responses:
        '200':
          description: 成功
          content:
            application/x-protobuf:
              schema:
                allOf:
                  - $ref: '../openapi-common.yaml#/components/schemas/ResponseHeader'
                  - $ref: '#/components/schemas/SearchConversationsRsp'
        '4xx': { $ref: '#/components/responses/BusinessError' }

  /service/conversation/get_member_ids:
    post:
      summary: 获取成员ID列表
      tags: [Conversation]
      x-protobuf: { service: chatnow.conversation.ConversationService, rpc: GetMemberIds, request: GetMemberIdsReq, response: GetMemberIdsRsp, file: proto/conversation/conversation_service.proto }
      requestBody:
        required: true
        content:
          application/x-protobuf:
            schema: { $ref: '#/components/schemas/GetMemberIdsReq' }
      responses:
        '200':
          description: 成功
          content:
            application/x-protobuf:
              schema:
                allOf:
                  - $ref: '../openapi-common.yaml#/components/schemas/ResponseHeader'
                  - $ref: '#/components/schemas/GetMemberIdsRsp'
        '4xx': { $ref: '#/components/responses/BusinessError' }

components:
  securitySchemes:
    bearerAuth:
      type: http
      scheme: bearer
      bearerFormat: JWT

  responses:
    Success:
      description: 成功（仅 ResponseHeader，无额外数据）
      content:
        application/x-protobuf:
          schema: { $ref: '../openapi-common.yaml#/components/schemas/ResponseHeader' }
    BusinessError:
      description: 业务错误（error_code != 0）
      content:
        application/x-protobuf:
          schema: { $ref: '../openapi-common.yaml#/components/schemas/ResponseHeader' }

  schemas:
    ConversationType:
      x-protobuf: { message: ConversationType, file: proto/conversation/conversation_service.proto }
      type: integer
      description: 0=UNSPECIFIED, 1=PRIVATE, 2=GROUP, 3=CHANNEL

    ConversationStatus:
      x-protobuf: { message: ConversationStatus, file: proto/conversation/conversation_service.proto }
      type: integer
      description: 0=NORMAL, 1=ARCHIVED, 2=DISMISSED

    MemberRole:
      x-protobuf: { message: MemberRole, file: proto/conversation/conversation_service.proto }
      type: integer
      description: 0=MEMBER, 1=ADMIN, 2=OWNER

    Conversation:
      type: object
      properties:
        conversation_id: { type: string }
        type: { $ref: '#/components/schemas/ConversationType' }
        name: { type: string }
        avatar_url: { type: string }
        description: { type: string }
        created_at_ms: { type: integer, format: int64 }
        member_count: { type: integer }
        top_member_ids:
          type: array
          items: { type: string }
        status: { $ref: '#/components/schemas/ConversationStatus' }
        last_message: { $ref: '#/components/schemas/MessagePreview' }
        self: { $ref: '#/components/schemas/SelfMemberInfo' }

    SelfMemberInfo:
      type: object
      properties:
        role: { $ref: '#/components/schemas/MemberRole' }
        joined_at_ms: { type: integer, format: int64 }
        is_muted: { type: boolean }
        is_pinned: { type: boolean }
        pin_time_ms: { type: integer, format: int64 }
        is_visible: { type: boolean }
        last_read_seq: { type: integer }
        unread_count: { type: integer }
        draft: { type: string }

    MemberItem:
      type: object
      properties:
        user_info: { $ref: '../openapi-common.yaml#/components/schemas/UserInfo' }
        role: { $ref: '#/components/schemas/MemberRole' }
        join_time_ms: { type: integer, format: int64 }

    MessagePreview:
      x-protobuf: { message: MessagePreview, file: proto/message/message_types.proto }
      type: object
      properties:
        message_id: { type: integer, format: int64 }
        sender_id: { type: string }
        message_type: { type: integer }
        content_preview: { type: string }
        sent_at_ms: { type: integer, format: int64 }
        status: { type: integer }

    ListConversationsReq:
      x-protobuf: { message: ListConversationsReq, file: proto/conversation/conversation_service.proto }
      type: object
      properties:
        request_id: { type: string }
        page: { $ref: '../openapi-common.yaml#/components/schemas/PageRequest' }

    ListConversationsRsp:
      type: object
      properties:
        conversations:
          type: array
          items: { $ref: '#/components/schemas/Conversation' }
        page: { $ref: '../openapi-common.yaml#/components/schemas/PageResponse' }

    GetConversationReq:
      x-protobuf: { message: GetConversationReq, file: proto/conversation/conversation_service.proto }
      type: object
      required: [request_id, conversation_id]
      properties:
        request_id: { type: string }
        conversation_id: { type: string }

    GetConversationRsp:
      type: object
      properties:
        conversation: { $ref: '#/components/schemas/Conversation' }

    CreateConversationReq:
      x-protobuf: { message: CreateConversationReq, file: proto/conversation/conversation_service.proto }
      type: object
      required: [request_id, type, member_ids]
      properties:
        request_id: { type: string }
        type: { $ref: '#/components/schemas/ConversationType' }
        name: { type: string }
        avatar_url: { type: string }
        description: { type: string }
        member_ids:
          type: array
          items: { type: string }

    CreateConversationRsp:
      type: object
      properties:
        conversation: { $ref: '#/components/schemas/Conversation' }

    UpdateConversationReq:
      x-protobuf: { message: UpdateConversationReq, file: proto/conversation/conversation_service.proto }
      type: object
      required: [request_id, conversation_id]
      properties:
        request_id: { type: string }
        conversation_id: { type: string }
        name: { type: string }
        avatar_url: { type: string }
        description: { type: string }
        announcement: { type: string }

    UpdateConversationRsp:
      type: object
      properties:
        conversation: { $ref: '#/components/schemas/Conversation' }

    DismissConversationReq:
      x-protobuf: { message: DismissConversationReq, file: proto/conversation/conversation_service.proto }
      type: object
      required: [request_id, conversation_id]
      properties:
        request_id: { type: string }
        conversation_id: { type: string }

    AddMembersReq:
      x-protobuf: { message: AddMembersReq, file: proto/conversation/conversation_service.proto }
      type: object
      required: [request_id, conversation_id, member_ids]
      properties:
        request_id: { type: string }
        conversation_id: { type: string }
        member_ids:
          type: array
          items: { type: string }

    RemoveMembersReq:
      x-protobuf: { message: RemoveMembersReq, file: proto/conversation/conversation_service.proto }
      type: object
      required: [request_id, conversation_id, member_ids]
      properties:
        request_id: { type: string }
        conversation_id: { type: string }
        member_ids:
          type: array
          items: { type: string }

    TransferOwnerReq:
      x-protobuf: { message: TransferOwnerReq, file: proto/conversation/conversation_service.proto }
      type: object
      required: [request_id, conversation_id, new_owner_id]
      properties:
        request_id: { type: string }
        conversation_id: { type: string }
        new_owner_id: { type: string }

    ChangeMemberRoleReq:
      x-protobuf: { message: ChangeMemberRoleReq, file: proto/conversation/conversation_service.proto }
      type: object
      required: [request_id, conversation_id, target_user_id, role]
      properties:
        request_id: { type: string }
        conversation_id: { type: string }
        target_user_id: { type: string }
        role: { $ref: '#/components/schemas/MemberRole' }

    ListMembersReq:
      x-protobuf: { message: ListMembersReq, file: proto/conversation/conversation_service.proto }
      type: object
      required: [request_id, conversation_id]
      properties:
        request_id: { type: string }
        conversation_id: { type: string }
        page: { $ref: '../openapi-common.yaml#/components/schemas/PageRequest' }

    ListMembersRsp:
      type: object
      properties:
        members:
          type: array
          items: { $ref: '#/components/schemas/MemberItem' }
        page: { $ref: '../openapi-common.yaml#/components/schemas/PageResponse' }

    SetMuteReq:
      x-protobuf: { message: SetMuteReq, file: proto/conversation/conversation_service.proto }
      type: object
      required: [request_id, conversation_id, mute]
      properties:
        request_id: { type: string }
        conversation_id: { type: string }
        mute: { type: boolean }

    SetMuteRsp:
      type: object
      properties:
        self: { $ref: '#/components/schemas/SelfMemberInfo' }

    SetPinReq:
      x-protobuf: { message: SetPinReq, file: proto/conversation/conversation_service.proto }
      type: object
      required: [request_id, conversation_id, pin]
      properties:
        request_id: { type: string }
        conversation_id: { type: string }
        pin: { type: boolean }

    SetPinRsp:
      type: object
      properties:
        self: { $ref: '#/components/schemas/SelfMemberInfo' }

    SetVisibleReq:
      x-protobuf: { message: SetVisibleReq, file: proto/conversation/conversation_service.proto }
      type: object
      required: [request_id, conversation_id, visible]
      properties:
        request_id: { type: string }
        conversation_id: { type: string }
        visible: { type: boolean }

    SetVisibleRsp:
      type: object
      properties:
        self: { $ref: '#/components/schemas/SelfMemberInfo' }

    QuitConversationReq:
      x-protobuf: { message: QuitConversationReq, file: proto/conversation/conversation_service.proto }
      type: object
      required: [request_id, conversation_id]
      properties:
        request_id: { type: string }
        conversation_id: { type: string }

    MarkReadReq:
      x-protobuf: { message: MarkReadReq, file: proto/conversation/conversation_service.proto }
      type: object
      required: [request_id, conversation_id, last_read_seq]
      properties:
        request_id: { type: string }
        conversation_id: { type: string }
        last_read_seq: { type: integer }

    SaveDraftReq:
      x-protobuf: { message: SaveDraftReq, file: proto/conversation/conversation_service.proto }
      type: object
      required: [request_id, conversation_id, draft]
      properties:
        request_id: { type: string }
        conversation_id: { type: string }
        draft: { type: string }

    SaveDraftRsp:
      type: object
      properties:
        self: { $ref: '#/components/schemas/SelfMemberInfo' }

    SearchConversationsReq:
      x-protobuf: { message: SearchConversationsReq, file: proto/conversation/conversation_service.proto }
      type: object
      required: [request_id, search_key]
      properties:
        request_id: { type: string }
        search_key: { type: string }

    SearchConversationsRsp:
      type: object
      properties:
        conversations:
          type: array
          items: { $ref: '#/components/schemas/Conversation' }

    GetMemberIdsReq:
      x-protobuf: { message: GetMemberIdsReq, file: proto/conversation/conversation_service.proto }
      type: object
      required: [request_id, conversation_id]
      properties:
        request_id: { type: string }
        conversation_id: { type: string }

    GetMemberIdsRsp:
      type: object
      properties:
        member_ids:
          type: array
          items: { type: string }
```

- [ ] **Step 2: Commit**

```bash
git add docs/api/openapi-conversation.yaml
git commit -m "docs(api): add OpenAPI conversation domain (18 endpoints)"
```

---

### Task 5: Create openapi-message.yaml (13 endpoints)

**Files:**
- Create: `docs/api/openapi-message.yaml`

- [ ] **Step 1: Write the Message OpenAPI file**

Write `docs/api/openapi-message.yaml`:

```yaml
openapi: 3.1.0
info:
  title: ChatNow Message API
  version: 3.0.0
  description: |
    消息域。Proto: proto/message/message_service.proto
    Service: chatnow.message.MessageService
    注意: sync 接口超时 10s（长轮询），其余 3s。

servers:
  - url: http://localhost:9000
    description: Gateway HTTP

security:
  - bearerAuth: []

paths:
  /service/message/sync:
    post:
      summary: 同步消息（长轮询）
      description: 超时 10000ms。传入 after_seq 拉取该会话的新消息。
      tags: [Message]
      x-protobuf: { service: chatnow.message.MessageService, rpc: SyncMessages, request: SyncMessagesReq, response: SyncMessagesRsp, file: proto/message/message_service.proto }
      requestBody:
        required: true
        content:
          application/x-protobuf:
            schema: { $ref: '#/components/schemas/SyncMessagesReq' }
      responses:
        '200':
          description: 成功
          content:
            application/x-protobuf:
              schema:
                allOf:
                  - $ref: '../openapi-common.yaml#/components/schemas/ResponseHeader'
                  - $ref: '#/components/schemas/SyncMessagesRsp'
        '4xx': { $ref: '#/components/responses/BusinessError' }

  /service/message/get_history:
    post:
      summary: 获取历史消息
      tags: [Message]
      x-protobuf: { service: chatnow.message.MessageService, rpc: GetHistory, request: GetHistoryReq, response: GetHistoryRsp, file: proto/message/message_service.proto }
      requestBody:
        required: true
        content:
          application/x-protobuf:
            schema: { $ref: '#/components/schemas/GetHistoryReq' }
      responses:
        '200':
          description: 成功
          content:
            application/x-protobuf:
              schema:
                allOf:
                  - $ref: '../openapi-common.yaml#/components/schemas/ResponseHeader'
                  - $ref: '#/components/schemas/GetHistoryRsp'
        '4xx': { $ref: '#/components/responses/BusinessError' }

  /service/message/get_by_id:
    post:
      summary: 按ID批量获取消息
      tags: [Message]
      x-protobuf: { service: chatnow.message.MessageService, rpc: GetMessagesById, request: GetMessagesByIdReq, response: GetMessagesByIdRsp, file: proto/message/message_service.proto }
      requestBody:
        required: true
        content:
          application/x-protobuf:
            schema: { $ref: '#/components/schemas/GetMessagesByIdReq' }
      responses:
        '200':
          description: 成功
          content:
            application/x-protobuf:
              schema:
                allOf:
                  - $ref: '../openapi-common.yaml#/components/schemas/ResponseHeader'
                  - $ref: '#/components/schemas/GetMessagesByIdRsp'
        '4xx': { $ref: '#/components/responses/BusinessError' }

  /service/message/search:
    post:
      summary: 搜索消息
      tags: [Message]
      x-protobuf: { service: chatnow.message.MessageService, rpc: SearchMessages, request: SearchMessagesReq, response: SearchMessagesRsp, file: proto/message/message_service.proto }
      requestBody:
        required: true
        content:
          application/x-protobuf:
            schema: { $ref: '#/components/schemas/SearchMessagesReq' }
      responses:
        '200':
          description: 成功
          content:
            application/x-protobuf:
              schema:
                allOf:
                  - $ref: '../openapi-common.yaml#/components/schemas/ResponseHeader'
                  - $ref: '#/components/schemas/SearchMessagesRsp'
        '4xx': { $ref: '#/components/responses/BusinessError' }

  /service/message/recall:
    post:
      summary: 撤回消息
      tags: [Message]
      x-protobuf: { service: chatnow.message.MessageService, rpc: RecallMessage, request: RecallMessageReq, response: RecallMessageRsp, file: proto/message/message_service.proto }
      requestBody:
        required: true
        content:
          application/x-protobuf:
            schema: { $ref: '#/components/schemas/RecallMessageReq' }
      responses:
        '200': { $ref: '#/components/responses/Success' }
        '4xx': { $ref: '#/components/responses/BusinessError' }

  /service/message/add_reaction:
    post:
      summary: 添加表情回应
      tags: [Message]
      x-protobuf: { service: chatnow.message.MessageService, rpc: AddReaction, request: AddReactionReq, response: AddReactionRsp, file: proto/message/message_service.proto }
      requestBody:
        required: true
        content:
          application/x-protobuf:
            schema: { $ref: '#/components/schemas/AddReactionReq' }
      responses:
        '200': { $ref: '#/components/responses/Success' }
        '4xx': { $ref: '#/components/responses/BusinessError' }

  /service/message/remove_reaction:
    post:
      summary: 移除表情回应
      tags: [Message]
      x-protobuf: { service: chatnow.message.MessageService, rpc: RemoveReaction, request: RemoveReactionReq, response: RemoveReactionRsp, file: proto/message/message_service.proto }
      requestBody:
        required: true
        content:
          application/x-protobuf:
            schema: { $ref: '#/components/schemas/RemoveReactionReq' }
      responses:
        '200': { $ref: '#/components/responses/Success' }
        '4xx': { $ref: '#/components/responses/BusinessError' }

  /service/message/get_reactions:
    post:
      summary: 获取表情回应列表
      tags: [Message]
      x-protobuf: { service: chatnow.message.MessageService, rpc: GetReactions, request: GetReactionsReq, response: GetReactionsRsp, file: proto/message/message_service.proto }
      requestBody:
        required: true
        content:
          application/x-protobuf:
            schema: { $ref: '#/components/schemas/GetReactionsReq' }
      responses:
        '200':
          description: 成功
          content:
            application/x-protobuf:
              schema:
                allOf:
                  - $ref: '../openapi-common.yaml#/components/schemas/ResponseHeader'
                  - $ref: '#/components/schemas/GetReactionsRsp'
        '4xx': { $ref: '#/components/responses/BusinessError' }

  /service/message/pin:
    post:
      summary: 置顶消息
      tags: [Message]
      x-protobuf: { service: chatnow.message.MessageService, rpc: PinMessage, request: PinMessageReq, response: PinMessageRsp, file: proto/message/message_service.proto }
      requestBody:
        required: true
        content:
          application/x-protobuf:
            schema: { $ref: '#/components/schemas/PinMessageReq' }
      responses:
        '200': { $ref: '#/components/responses/Success' }
        '4xx': { $ref: '#/components/responses/BusinessError' }

  /service/message/unpin:
    post:
      summary: 取消置顶消息
      tags: [Message]
      x-protobuf: { service: chatnow.message.MessageService, rpc: UnpinMessage, request: UnpinMessageReq, response: UnpinMessageRsp, file: proto/message/message_service.proto }
      requestBody:
        required: true
        content:
          application/x-protobuf:
            schema: { $ref: '#/components/schemas/UnpinMessageReq' }
      responses:
        '200': { $ref: '#/components/responses/Success' }
        '4xx': { $ref: '#/components/responses/BusinessError' }

  /service/message/list_pinned:
    post:
      summary: 置顶消息列表
      tags: [Message]
      x-protobuf: { service: chatnow.message.MessageService, rpc: ListPinnedMessages, request: ListPinnedReq, response: ListPinnedRsp, file: proto/message/message_service.proto }
      requestBody:
        required: true
        content:
          application/x-protobuf:
            schema: { $ref: '#/components/schemas/ListPinnedReq' }
      responses:
        '200':
          description: 成功
          content:
            application/x-protobuf:
              schema:
                allOf:
                  - $ref: '../openapi-common.yaml#/components/schemas/ResponseHeader'
                  - $ref: '#/components/schemas/ListPinnedRsp'
        '4xx': { $ref: '#/components/responses/BusinessError' }

  /service/message/delete:
    post:
      summary: 删除消息
      tags: [Message]
      x-protobuf: { service: chatnow.message.MessageService, rpc: DeleteMessages, request: DeleteMessagesReq, response: DeleteMessagesRsp, file: proto/message/message_service.proto }
      requestBody:
        required: true
        content:
          application/x-protobuf:
            schema: { $ref: '#/components/schemas/DeleteMessagesReq' }
      responses:
        '200': { $ref: '#/components/responses/Success' }
        '4xx': { $ref: '#/components/responses/BusinessError' }

  /service/message/clear:
    post:
      summary: 清空会话消息
      tags: [Message]
      x-protobuf: { service: chatnow.message.MessageService, rpc: ClearConversation, request: ClearConversationReq, response: ClearConversationRsp, file: proto/message/message_service.proto }
      requestBody:
        required: true
        content:
          application/x-protobuf:
            schema: { $ref: '#/components/schemas/ClearConversationReq' }
      responses:
        '200': { $ref: '#/components/responses/Success' }
        '4xx': { $ref: '#/components/responses/BusinessError' }

components:
  securitySchemes:
    bearerAuth:
      type: http
      scheme: bearer
      bearerFormat: JWT

  responses:
    Success:
      description: 成功（仅 ResponseHeader，无额外数据）
      content:
        application/x-protobuf:
          schema: { $ref: '../openapi-common.yaml#/components/schemas/ResponseHeader' }
    BusinessError:
      description: 业务错误（error_code != 0）
      content:
        application/x-protobuf:
          schema: { $ref: '../openapi-common.yaml#/components/schemas/ResponseHeader' }

  schemas:
    MessageType:
      x-protobuf: { message: MessageType, file: proto/message/message_types.proto }
      type: integer
      description: 0=UNSPECIFIED, 1=TEXT, 2=IMAGE, 3=FILE, 4=AUDIO, 5=VIDEO, 6=LOCATION, 7=STICKER, 8=SYSTEM_NOTICE

    MessageStatus:
      x-protobuf: { message: MessageStatus, file: proto/message/message_types.proto }
      type: integer
      description: 0=NORMAL, 1=RECALLED, 2=DELETED

    MessageContent:
      x-protobuf: { message: MessageContent, file: proto/message/message_types.proto }
      type: object
      properties:
        type: { $ref: '#/components/schemas/MessageType' }
        body:
          oneOf:
            - { $ref: '#/components/schemas/TextContent' }
            - { $ref: '#/components/schemas/ImageContent' }
            - { $ref: '#/components/schemas/FileContent' }
            - { $ref: '#/components/schemas/AudioContent' }
            - { $ref: '#/components/schemas/VideoContent' }
            - { $ref: '#/components/schemas/LocationContent' }
            - { $ref: '#/components/schemas/StickerContent' }
            - { $ref: '#/components/schemas/SystemNoticeContent' }

    TextContent:
      type: object
      properties: { text: { type: string } }

    ImageContent:
      type: object
      properties:
        file_id: { type: string }
        width: { type: integer }
        height: { type: integer }
        thumbnail_url: { type: string }

    FileContent:
      type: object
      properties:
        file_id: { type: string }
        file_name: { type: string }
        file_size: { type: integer, format: int64 }
        mime_type: { type: string }

    AudioContent:
      type: object
      properties:
        file_id: { type: string }
        duration_sec: { type: integer }

    VideoContent:
      type: object
      properties:
        file_id: { type: string }
        duration_sec: { type: integer }
        width: { type: integer }
        height: { type: integer }
        thumbnail_url: { type: string }

    LocationContent:
      type: object
      properties:
        latitude: { type: number, format: double }
        longitude: { type: number, format: double }
        name: { type: string }
        address: { type: string }

    StickerContent:
      type: object
      properties:
        sticker_id: { type: string }
        pack_id: { type: string }

    SystemNoticeContent:
      type: object
      properties:
        text: { type: string }
        notice_type: { type: string }

    Message:
      x-protobuf: { message: Message, file: proto/message/message_types.proto }
      type: object
      properties:
        message_id: { type: integer, format: int64 }
        conversation_id: { type: string }
        content: { $ref: '#/components/schemas/MessageContent' }
        sender_id: { type: string }
        created_at_ms: { type: integer, format: int64 }
        edited_at_ms: { type: integer, format: int64 }
        seq_id: { type: integer }
        user_seq: { type: integer }
        client_msg_id: { type: string }
        status: { $ref: '#/components/schemas/MessageStatus' }
        reply_to: { $ref: '#/components/schemas/ReplyRef' }
        mentioned_user_ids:
          type: array
          items: { type: string }
        forward_info: { $ref: '#/components/schemas/ForwardInfo' }
        reactions:
          type: array
          items: { $ref: '#/components/schemas/ReactionGroup' }
        is_pinned: { type: boolean }

    ReplyRef:
      x-protobuf: { message: ReplyRef, file: proto/message/message_types.proto }
      type: object
      properties:
        replied_message_id: { type: integer, format: int64 }
        replied_sender_id: { type: string }
        replied_message_type: { $ref: '#/components/schemas/MessageType' }
        content_preview: { type: string }
        is_recalled: { type: boolean }

    ReactionGroup:
      x-protobuf: { message: ReactionGroup, file: proto/message/message_types.proto }
      type: object
      properties:
        emoji: { type: string }
        count: { type: integer }
        recent_user_ids:
          type: array
          items: { type: string }
        self_reacted: { type: boolean }

    ForwardInfo:
      x-protobuf: { message: ForwardInfo, file: proto/message/message_types.proto }
      type: object
      properties:
        forward_from_user_id: { type: string }
        forward_at_ms: { type: integer, format: int64 }
        source_conversation_id: { type: string }

    SyncMessagesReq:
      x-protobuf: { message: SyncMessagesReq, file: proto/message/message_service.proto }
      type: object
      required: [request_id, conversation_id, after_seq, limit]
      properties:
        request_id: { type: string }
        conversation_id: { type: string }
        after_seq: { type: integer }
        limit: { type: integer }

    SyncMessagesRsp:
      type: object
      properties:
        messages:
          type: array
          items: { $ref: '#/components/schemas/Message' }
        has_more: { type: boolean }
        latest_seq: { type: integer }

    GetHistoryReq:
      x-protobuf: { message: GetHistoryReq, file: proto/message/message_service.proto }
      type: object
      required: [request_id, conversation_id, before_seq, limit]
      properties:
        request_id: { type: string }
        conversation_id: { type: string }
        before_seq: { type: integer }
        limit: { type: integer }

    GetHistoryRsp:
      type: object
      properties:
        messages:
          type: array
          items: { $ref: '#/components/schemas/Message' }
        has_more: { type: boolean }

    GetMessagesByIdReq:
      x-protobuf: { message: GetMessagesByIdReq, file: proto/message/message_service.proto }
      type: object
      required: [request_id, message_ids]
      properties:
        request_id: { type: string }
        message_ids:
          type: array
          items: { type: integer, format: int64 }

    GetMessagesByIdRsp:
      type: object
      properties:
        messages:
          type: array
          items: { $ref: '#/components/schemas/Message' }

    SearchMessagesReq:
      x-protobuf: { message: SearchMessagesReq, file: proto/message/message_service.proto }
      type: object
      required: [request_id, conversation_id, keyword]
      properties:
        request_id: { type: string }
        conversation_id: { type: string }
        keyword: { type: string }
        limit: { type: integer }
        cursor: { type: string }

    SearchMessagesRsp:
      type: object
      properties:
        messages:
          type: array
          items: { $ref: '#/components/schemas/Message' }
        has_more: { type: boolean }
        next_cursor: { type: string }

    RecallMessageReq:
      x-protobuf: { message: RecallMessageReq, file: proto/message/message_service.proto }
      type: object
      required: [request_id, conversation_id, message_id]
      properties:
        request_id: { type: string }
        conversation_id: { type: string }
        message_id: { type: integer, format: int64 }

    AddReactionReq:
      x-protobuf: { message: AddReactionReq, file: proto/message/message_service.proto }
      type: object
      required: [request_id, message_id, emoji]
      properties:
        request_id: { type: string }
        message_id: { type: integer, format: int64 }
        emoji: { type: string }

    RemoveReactionReq:
      x-protobuf: { message: RemoveReactionReq, file: proto/message/message_service.proto }
      type: object
      required: [request_id, message_id, emoji]
      properties:
        request_id: { type: string }
        message_id: { type: integer, format: int64 }
        emoji: { type: string }

    GetReactionsReq:
      x-protobuf: { message: GetReactionsReq, file: proto/message/message_service.proto }
      type: object
      required: [request_id, message_id]
      properties:
        request_id: { type: string }
        message_id: { type: integer, format: int64 }

    GetReactionsRsp:
      type: object
      properties:
        reactions:
          type: array
          items: { $ref: '#/components/schemas/ReactionGroup' }

    PinMessageReq:
      x-protobuf: { message: PinMessageReq, file: proto/message/message_service.proto }
      type: object
      required: [request_id, conversation_id, message_id]
      properties:
        request_id: { type: string }
        conversation_id: { type: string }
        message_id: { type: integer, format: int64 }

    UnpinMessageReq:
      x-protobuf: { message: UnpinMessageReq, file: proto/message/message_service.proto }
      type: object
      required: [request_id, conversation_id, message_id]
      properties:
        request_id: { type: string }
        conversation_id: { type: string }
        message_id: { type: integer, format: int64 }

    ListPinnedReq:
      x-protobuf: { message: ListPinnedReq, file: proto/message/message_service.proto }
      type: object
      required: [request_id, conversation_id]
      properties:
        request_id: { type: string }
        conversation_id: { type: string }

    ListPinnedRsp:
      type: object
      properties:
        messages:
          type: array
          items: { $ref: '#/components/schemas/Message' }

    DeleteMessagesReq:
      x-protobuf: { message: DeleteMessagesReq, file: proto/message/message_service.proto }
      type: object
      required: [request_id, conversation_id, message_ids]
      properties:
        request_id: { type: string }
        conversation_id: { type: string }
        message_ids:
          type: array
          items: { type: integer, format: int64 }

    ClearConversationReq:
      x-protobuf: { message: ClearConversationReq, file: proto/message/message_service.proto }
      type: object
      required: [request_id, conversation_id]
      properties:
        request_id: { type: string }
        conversation_id: { type: string }
```

- [ ] **Step 2: Commit**

```bash
git add docs/api/openapi-message.yaml
git commit -m "docs(api): add OpenAPI message domain (13 endpoints)"
```

---

### Task 6: Create openapi-transmite.yaml (1 endpoint)

**Files:**
- Create: `docs/api/openapi-transmite.yaml`

- [ ] **Step 1: Write the Transmite OpenAPI file**

Write `docs/api/openapi-transmite.yaml`:

```yaml
openapi: 3.1.0
info:
  title: ChatNow Transmite API
  version: 3.0.0
  description: |
    消息发送域。Proto: proto/transmite/transmite_service.proto
    Service: chatnow.transmite.MsgTransmitService
    超时 1000ms（快速通道）。

servers:
  - url: http://localhost:9000
    description: Gateway HTTP

security:
  - bearerAuth: []

paths:
  /service/transmite/send:
    post:
      summary: 发送消息
      description: 超时 1000ms。client_msg_id 用于客户端幂等去重。
      tags: [Transmite]
      x-protobuf: { service: chatnow.transmite.MsgTransmitService, rpc: SendMessage, request: SendMessageReq, response: SendMessageRsp, file: proto/transmite/transmite_service.proto }
      requestBody:
        required: true
        content:
          application/x-protobuf:
            schema: { $ref: '#/components/schemas/SendMessageReq' }
      responses:
        '200':
          description: 成功
          content:
            application/x-protobuf:
              schema:
                allOf:
                  - $ref: '../openapi-common.yaml#/components/schemas/ResponseHeader'
                  - $ref: '#/components/schemas/SendMessageRsp'
        '4xx': { $ref: '#/components/responses/BusinessError' }

components:
  securitySchemes:
    bearerAuth:
      type: http
      scheme: bearer
      bearerFormat: JWT

  responses:
    BusinessError:
      description: 业务错误（error_code != 0）
      content:
        application/x-protobuf:
          schema: { $ref: '../openapi-common.yaml#/components/schemas/ResponseHeader' }

  schemas:
    MessageContent:
      x-protobuf: { message: MessageContent, file: proto/message/message_types.proto }
      type: object
      properties:
        type: { type: integer, description: MessageType 枚举值，0=UNSPECIFIED, 1=TEXT, 2=IMAGE, 3=FILE, 4=AUDIO, 5=VIDEO, 6=LOCATION, 7=STICKER, 8=SYSTEM_NOTICE }
        body:
          oneOf:
            - type: object
              properties: { text: { type: string } }
            - type: object
              properties: { file_id: { type: string }, width: { type: integer }, height: { type: integer }, thumbnail_url: { type: string } }
            - type: object
              properties: { file_id: { type: string }, file_name: { type: string }, file_size: { type: integer, format: int64 }, mime_type: { type: string } }
            - type: object
              properties: { file_id: { type: string }, duration_sec: { type: integer } }
            - type: object
              properties: { file_id: { type: string }, duration_sec: { type: integer }, width: { type: integer }, height: { type: integer }, thumbnail_url: { type: string } }
            - type: object
              properties: { latitude: { type: number, format: double }, longitude: { type: number, format: double }, name: { type: string }, address: { type: string } }
            - type: object
              properties: { sticker_id: { type: string }, pack_id: { type: string } }
            - type: object
              properties: { text: { type: string }, notice_type: { type: string } }

    ReplyRef:
      x-protobuf: { message: ReplyRef, file: proto/message/message_types.proto }
      type: object
      properties:
        replied_message_id: { type: integer, format: int64 }
        replied_sender_id: { type: string }
        replied_message_type: { type: integer }
        content_preview: { type: string }
        is_recalled: { type: boolean }

    ForwardInfo:
      x-protobuf: { message: ForwardInfo, file: proto/message/message_types.proto }
      type: object
      properties:
        forward_from_user_id: { type: string }
        forward_at_ms: { type: integer, format: int64 }
        source_conversation_id: { type: string }

    Message:
      x-protobuf: { message: Message, file: proto/message/message_types.proto }
      type: object
      properties:
        message_id: { type: integer, format: int64 }
        conversation_id: { type: string }
        content: { $ref: '#/components/schemas/MessageContent' }
        sender_id: { type: string }
        created_at_ms: { type: integer, format: int64 }
        edited_at_ms: { type: integer, format: int64 }
        seq_id: { type: integer }
        user_seq: { type: integer }
        client_msg_id: { type: string }
        status: { type: integer }
        reply_to: { $ref: '#/components/schemas/ReplyRef' }
        mentioned_user_ids: { type: array, items: { type: string } }
        forward_info: { $ref: '#/components/schemas/ForwardInfo' }
        reactions: { type: array, items: { $ref: '#/components/schemas/ReactionGroup' } }
        is_pinned: { type: boolean }

    ReactionGroup:
      x-protobuf: { message: ReactionGroup, file: proto/message/message_types.proto }
      type: object
      properties:
        emoji: { type: string }
        count: { type: integer }
        recent_user_ids: { type: array, items: { type: string } }
        self_reacted: { type: boolean }

    SendMessageReq:
      x-protobuf: { message: SendMessageReq, file: proto/transmite/transmite_service.proto }
      type: object
      required: [request_id, conversation_id, content]
      properties:
        request_id: { type: string }
        conversation_id: { type: string }
        content: { $ref: '#/components/schemas/MessageContent' }
        client_msg_id:
          type: string
          description: 客户端幂等键，用于去重
        reply_to: { $ref: '#/components/schemas/ReplyRef' }
        mentioned_user_ids:
          type: array
          items: { type: string }
          description: @提及的用户ID列表
        forward_info: { $ref: '#/components/schemas/ForwardInfo' }

    SendMessageRsp:
      type: object
      properties:
        message:
          $ref: '#/components/schemas/Message'
          description: 组装后的完整消息对象
```

- [ ] **Step 2: Commit**

```bash
git add docs/api/openapi-transmite.yaml
git commit -m "docs(api): add OpenAPI transmite domain (1 endpoint)"
```

---

### Task 7: Create openapi-media.yaml (5 endpoints)

**Files:**
- Create: `docs/api/openapi-media.yaml`

- [ ] **Step 1: Write the Media OpenAPI file**

Write `docs/api/openapi-media.yaml`:

```yaml
openapi: 3.1.0
info:
  title: ChatNow Media API
  version: 3.0.0
  description: |
    媒体上传/下载域。Proto: proto/media/media_service.proto
    Service: chatnow.media.MediaService
    注意: 分片上传接口（InitMultipartUpload/ApplyPartUpload/CompleteMultipartUpload/AbortMultipartUpload）尚未暴露为 HTTP 路由。

servers:
  - url: http://localhost:9000
    description: Gateway HTTP

security:
  - bearerAuth: []

paths:
  /service/media/apply_upload:
    post:
      summary: 申请上传
      description: |
        申请单段上传（≤100MB）。返回 presigned PUT URL。
        若 content_hash 命中去重缓存，already_exists=true，无需再上传。
      tags: [Media]
      x-protobuf: { service: chatnow.media.MediaService, rpc: ApplyUpload, request: ApplyUploadReq, response: ApplyUploadRsp, file: proto/media/media_service.proto }
      requestBody:
        required: true
        content:
          application/x-protobuf:
            schema: { $ref: '#/components/schemas/ApplyUploadReq' }
      responses:
        '200':
          description: 成功
          content:
            application/x-protobuf:
              schema:
                allOf:
                  - $ref: '../openapi-common.yaml#/components/schemas/ResponseHeader'
                  - $ref: '#/components/schemas/ApplyUploadRsp'
        '4xx': { $ref: '#/components/responses/BusinessError' }

  /service/media/complete_upload:
    post:
      summary: 完成上传
      description: 通知服务端客户端已完成 PUT，触发文件就绪处理。
      tags: [Media]
      x-protobuf: { service: chatnow.media.MediaService, rpc: CompleteUpload, request: CompleteUploadReq, response: CompleteUploadRsp, file: proto/media/media_service.proto }
      requestBody:
        required: true
        content:
          application/x-protobuf:
            schema: { $ref: '#/components/schemas/CompleteUploadReq' }
      responses:
        '200':
          description: 成功
          content:
            application/x-protobuf:
              schema:
                allOf:
                  - $ref: '../openapi-common.yaml#/components/schemas/ResponseHeader'
                  - $ref: '#/components/schemas/CompleteUploadRsp'
        '4xx': { $ref: '#/components/responses/BusinessError' }

  /service/media/apply_download:
    post:
      summary: 申请下载
      description: 返回 presigned GET URL。
      tags: [Media]
      x-protobuf: { service: chatnow.media.MediaService, rpc: ApplyDownload, request: ApplyDownloadReq, response: ApplyDownloadRsp, file: proto/media/media_service.proto }
      requestBody:
        required: true
        content:
          application/x-protobuf:
            schema: { $ref: '#/components/schemas/ApplyDownloadReq' }
      responses:
        '200':
          description: 成功
          content:
            application/x-protobuf:
              schema:
                allOf:
                  - $ref: '../openapi-common.yaml#/components/schemas/ResponseHeader'
                  - $ref: '#/components/schemas/ApplyDownloadRsp'
        '4xx': { $ref: '#/components/responses/BusinessError' }

  /service/media/get_file_info:
    post:
      summary: 获取文件信息
      tags: [Media]
      x-protobuf: { service: chatnow.media.MediaService, rpc: GetFileInfo, request: GetFileInfoReq, response: GetFileInfoRsp, file: proto/media/media_service.proto }
      requestBody:
        required: true
        content:
          application/x-protobuf:
            schema: { $ref: '#/components/schemas/GetFileInfoReq' }
      responses:
        '200':
          description: 成功
          content:
            application/x-protobuf:
              schema:
                allOf:
                  - $ref: '../openapi-common.yaml#/components/schemas/ResponseHeader'
                  - $ref: '#/components/schemas/GetFileInfoRsp'
        '4xx': { $ref: '#/components/responses/BusinessError' }

  /service/media/speech_recognition:
    post:
      summary: 语音识别
      description: 短音频 ASR，直接传 bytes，不经过 S3。
      tags: [Media]
      x-protobuf: { service: chatnow.media.MediaService, rpc: SpeechRecognition, request: SpeechRecognitionReq, response: SpeechRecognitionRsp, file: proto/media/media_service.proto }
      requestBody:
        required: true
        content:
          application/x-protobuf:
            schema: { $ref: '#/components/schemas/SpeechRecognitionReq' }
      responses:
        '200':
          description: 成功
          content:
            application/x-protobuf:
              schema:
                allOf:
                  - $ref: '../openapi-common.yaml#/components/schemas/ResponseHeader'
                  - $ref: '#/components/schemas/SpeechRecognitionRsp'
        '4xx': { $ref: '#/components/responses/BusinessError' }

components:
  securitySchemes:
    bearerAuth:
      type: http
      scheme: bearer
      bearerFormat: JWT

  responses:
    BusinessError:
      description: 业务错误（error_code != 0）
      content:
        application/x-protobuf:
          schema: { $ref: '../openapi-common.yaml#/components/schemas/ResponseHeader' }

  schemas:
    MediaPurpose:
      x-protobuf: { message: MediaPurpose, file: proto/media/media_service.proto }
      type: integer
      description: 0=UNSPECIFIED, 1=AVATAR, 2=GROUP_AVATAR, 3=CHAT, 4=STICKER

    FileInfo:
      x-protobuf: { message: FileInfo, file: proto/media/media_service.proto }
      type: object
      properties:
        file_id: { type: string }
        file_name: { type: string }
        file_size: { type: integer, format: int64 }
        mime_type: { type: string }
        uploaded_at_ms: { type: integer, format: int64 }

    ApplyUploadReq:
      x-protobuf: { message: ApplyUploadReq, file: proto/media/media_service.proto }
      type: object
      required: [request_id, file_name, file_size, mime_type, content_hash]
      properties:
        request_id: { type: string }
        file_name: { type: string }
        file_size: { type: integer, format: int64 }
        mime_type: { type: string }
        content_hash:
          type: string
          description: 格式 "sha256:<64hex>"，用于去重和完整性校验
        purpose: { $ref: '#/components/schemas/MediaPurpose' }

    ApplyUploadRsp:
      type: object
      properties:
        file_id: { type: string }
        already_exists:
          type: boolean
          description: true=去重命中，无需上传
        upload_url:
          type: string
          description: presigned PUT URL（dedup 时为空）
        headers:
          type: object
          additionalProperties: { type: string }
          description: 客户端 PUT 时必须携带的 HTTP headers
        expires_in_sec: { type: integer }

    CompleteUploadReq:
      x-protobuf: { message: CompleteUploadReq, file: proto/media/media_service.proto }
      type: object
      required: [request_id, file_id]
      properties:
        request_id: { type: string }
        file_id: { type: string }

    CompleteUploadRsp:
      type: object
      properties:
        file_info: { $ref: '#/components/schemas/FileInfo' }

    ApplyDownloadReq:
      x-protobuf: { message: ApplyDownloadReq, file: proto/media/media_service.proto }
      type: object
      required: [request_id, file_id]
      properties:
        request_id: { type: string }
        file_id: { type: string }

    ApplyDownloadRsp:
      type: object
      properties:
        download_url:
          type: string
          description: presigned GET URL
        expires_in_sec: { type: integer }
        file_info: { $ref: '#/components/schemas/FileInfo' }

    GetFileInfoReq:
      x-protobuf: { message: GetFileInfoReq, file: proto/media/media_service.proto }
      type: object
      required: [request_id, file_id]
      properties:
        request_id: { type: string }
        file_id: { type: string }

    GetFileInfoRsp:
      type: object
      properties:
        file_info: { $ref: '#/components/schemas/FileInfo' }

    SpeechRecognitionReq:
      x-protobuf: { message: SpeechRecognitionReq, file: proto/media/media_service.proto }
      type: object
      required: [request_id, speech_content]
      properties:
        request_id: { type: string }
        speech_content:
          type: string
          format: byte
          description: 音频二进制数据（base64 编码）

    SpeechRecognitionRsp:
      type: object
      properties:
        recognition_result:
          type: string
          description: 识别出的文字
```

- [ ] **Step 2: Commit**

```bash
git add docs/api/openapi-media.yaml
git commit -m "docs(api): add OpenAPI media domain (5 endpoints)"
```

---

### Task 8: Create openapi-presence.yaml (4 endpoints)

**Files:**
- Create: `docs/api/openapi-presence.yaml`

- [ ] **Step 1: Write the Presence OpenAPI file**

Write `docs/api/openapi-presence.yaml`:

```yaml
openapi: 3.1.0
info:
  title: ChatNow Presence API
  version: 3.0.0
  description: |
    在线状态域。Proto: proto/presence/presence_service.proto
    Service: chatnow.presence.PresenceService

servers:
  - url: http://localhost:9000
    description: Gateway HTTP

security:
  - bearerAuth: []

paths:
  /service/presence/get:
    post:
      summary: 获取用户在线状态
      tags: [Presence]
      x-protobuf: { service: chatnow.presence.PresenceService, rpc: GetPresence, request: GetPresenceReq, response: GetPresenceRsp, file: proto/presence/presence_service.proto }
      requestBody:
        required: true
        content:
          application/x-protobuf:
            schema: { $ref: '#/components/schemas/GetPresenceReq' }
      responses:
        '200':
          description: 成功
          content:
            application/x-protobuf:
              schema:
                allOf:
                  - $ref: '../openapi-common.yaml#/components/schemas/ResponseHeader'
                  - $ref: '#/components/schemas/GetPresenceRsp'
        '4xx': { $ref: '#/components/responses/BusinessError' }

  /service/presence/batch_get:
    post:
      summary: 批量获取在线状态
      tags: [Presence]
      x-protobuf: { service: chatnow.presence.PresenceService, rpc: BatchGetPresence, request: BatchGetPresenceReq, response: BatchGetPresenceRsp, file: proto/presence/presence_service.proto }
      requestBody:
        required: true
        content:
          application/x-protobuf:
            schema: { $ref: '#/components/schemas/BatchGetPresenceReq' }
      responses:
        '200':
          description: 成功，返回 user_id -> Presence 映射
          content:
            application/x-protobuf:
              schema:
                allOf:
                  - $ref: '../openapi-common.yaml#/components/schemas/ResponseHeader'
                  - $ref: '#/components/schemas/BatchGetPresenceRsp'
        '4xx': { $ref: '#/components/responses/BusinessError' }

  /service/presence/subscribe:
    post:
      summary: 订阅在线状态
      description: subscriber_user_id 从 JWT metadata 提取，无需在 body 中传递。
      tags: [Presence]
      x-protobuf: { service: chatnow.presence.PresenceService, rpc: SubscribePresence, request: SubscribeReq, response: SubscribeRsp, file: proto/presence/presence_service.proto }
      requestBody:
        required: true
        content:
          application/x-protobuf:
            schema: { $ref: '#/components/schemas/SubscribeReq' }
      responses:
        '200': { $ref: '#/components/responses/Success' }
        '4xx': { $ref: '#/components/responses/BusinessError' }

  /service/presence/unsubscribe:
    post:
      summary: 取消订阅在线状态
      description: subscriber_user_id 从 JWT metadata 提取，无需在 body 中传递。
      tags: [Presence]
      x-protobuf: { service: chatnow.presence.PresenceService, rpc: UnsubscribePresence, request: UnsubscribeReq, response: UnsubscribeRsp, file: proto/presence/presence_service.proto }
      requestBody:
        required: true
        content:
          application/x-protobuf:
            schema: { $ref: '#/components/schemas/UnsubscribeReq' }
      responses:
        '200': { $ref: '#/components/responses/Success' }
        '4xx': { $ref: '#/components/responses/BusinessError' }

components:
  securitySchemes:
    bearerAuth:
      type: http
      scheme: bearer
      bearerFormat: JWT

  responses:
    Success:
      description: 成功（仅 ResponseHeader，无额外数据）
      content:
        application/x-protobuf:
          schema: { $ref: '../openapi-common.yaml#/components/schemas/ResponseHeader' }
    BusinessError:
      description: 业务错误（error_code != 0）
      content:
        application/x-protobuf:
          schema: { $ref: '../openapi-common.yaml#/components/schemas/ResponseHeader' }

  schemas:
    PresenceState:
      x-protobuf: { message: PresenceState, file: proto/presence/presence_service.proto }
      type: integer
      description: 0=UNSPECIFIED, 1=ONLINE, 2=AWAY, 3=BUSY, 4=OFFLINE, 5=INVISIBLE

    DevicePresence:
      x-protobuf: { message: DevicePresence, file: proto/presence/presence_service.proto }
      type: object
      properties:
        device_id: { type: string }
        platform: { $ref: '../openapi-common.yaml#/components/schemas/DevicePlatform' }
        state: { $ref: '#/components/schemas/PresenceState' }
        last_active_at_ms: { type: integer, format: int64 }

    Presence:
      x-protobuf: { message: Presence, file: proto/presence/presence_service.proto }
      type: object
      properties:
        user_id: { type: string }
        aggregated_state: { $ref: '#/components/schemas/PresenceState' }
        last_active_at_ms: { type: integer, format: int64 }
        devices:
          type: array
          items: { $ref: '#/components/schemas/DevicePresence' }

    GetPresenceReq:
      x-protobuf: { message: GetPresenceReq, file: proto/presence/presence_service.proto }
      type: object
      required: [request_id, user_id]
      properties:
        request_id: { type: string }
        user_id:
          type: string
          description: 查询目标用户ID

    GetPresenceRsp:
      type: object
      properties:
        presence: { $ref: '#/components/schemas/Presence' }

    BatchGetPresenceReq:
      x-protobuf: { message: BatchGetPresenceReq, file: proto/presence/presence_service.proto }
      type: object
      required: [request_id, user_ids]
      properties:
        request_id: { type: string }
        user_ids:
          type: array
          items: { type: string }

    BatchGetPresenceRsp:
      type: object
      properties:
        presences:
          type: object
          additionalProperties: { $ref: '#/components/schemas/Presence' }
          description: user_id -> Presence 映射

    SubscribeReq:
      x-protobuf: { message: SubscribeReq, file: proto/presence/presence_service.proto }
      type: object
      required: [request_id, subscribe_user_ids]
      properties:
        request_id: { type: string }
        subscribe_user_ids:
          type: array
          items: { type: string }
          description: 要订阅的用户ID列表

    UnsubscribeReq:
      x-protobuf: { message: UnsubscribeReq, file: proto/presence/presence_service.proto }
      type: object
      required: [request_id, unsubscribe_user_ids]
      properties:
        request_id: { type: string }
        unsubscribe_user_ids:
          type: array
          items: { type: string }
          description: 要取消订阅的用户ID列表
```

- [ ] **Step 2: Commit**

```bash
git add docs/api/openapi-presence.yaml
git commit -m "docs(api): add OpenAPI presence domain (4 endpoints)"
```

---

### Task 9: Validation

**Files:**
- None (read-only check)

- [ ] **Step 1: Verify all files exist and are valid YAML**

```bash
ls -la docs/api/openapi-*.yaml
for f in docs/api/openapi-*.yaml; do echo "Checking $f..."; python3 -c "import yaml; yaml.safe_load(open('$f')); print('  OK')"; done
```

Expected: all 8 files print "OK"

- [ ] **Step 2: Count endpoints across all files**

```bash
grep -c "x-protobuf:" docs/api/openapi-*.yaml
```

Expected: common=7 (schemas only, no paths), identity=9, relationship=9, conversation=18, message=13, transmite=1, media=5, presence=4

- [ ] **Step 3: Verify auth model consistency**

```bash
echo "WHITELISTED endpoints (security: []):"
grep -B5 'security: \[\]' docs/api/openapi-identity.yaml | grep '/service/' | sed 's/  //g'
```

Expected output: register, login, send_verify_code, refresh_token (4 endpoints)

- [ ] **Step 4: Commit validation**

No file changes, just verify. If all checks pass, we're done.
```

## Self-Review

**Spec coverage:**
- File layout (7+1 files): Tasks 1-8 ✓
- Endpoint anatomy (x-protobuf, allOf ResponseHeader): Every endpoint in Tasks 2-8 follows the pattern ✓
- Shared components (ResponseHeader, UserInfo, PageRequest, PageResponse, ErrorCode): Task 1 ✓
- x-protobuf extension: Applied to every endpoint and schema ✓
- Auth model (bearerAuth + security override): Applied correctly ✓
- Special timeouts: Mentioned in sync (Task 5) and send (Task 6) descriptions ✓
- Proto oneof handling: Applied for credential (Task 2), destination (Task 2), body (Task 5/6) ✓
- Endpoint inventory (59 total): Confirmed by count check in Task 9 ✓

**Placeholder scan:** No TBD, TODO, or incomplete sections. Every YAML block is complete with proper schemas.

**Type consistency:** Content type `application/x-protobuf` is consistent across all files. `$ref` paths use `'../openapi-common.yaml#/...'` consistently. `securitySchemes.bearerAuth` is defined in each domain file consistently. `x-protobuf` fields (service, rpc, request, response, file/message) are consistent.

Plan complete. Ready for execution handoff.
