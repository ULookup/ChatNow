# P2 JWT 鉴权手工烟雾测试

> 目的：验证 Identity 签发 + Gateway 验签链路在真实部署中工作。

## 前置

1. 启动依赖：MySQL / Redis / etcd（按 docker-compose）
2. 启动 `user_server`（10003）与 `gateway_server`（9000），二者共享 `/im/conf/auth.json`
3. 用 `user/test/user_client.cc` 提前注册一个测试用户

## 测试 1：Login 签发 access+refresh

POST `http://127.0.0.1:9000/service/user/username_login`

请求体（identity::LoginReq protobuf）：
- `username_pwd { username: "alice", password: "alice123" }`
- `device_id: "d1"`

期望：响应 `LoginRsp.tokens.access_token` 与 `refresh_token` 都非空，`header.success=true`，HTTP 200。

## 测试 2：用 access token 调一个鉴权接口（GetUserInfo）

```
POST /service/user/get_user_info
Authorization: Bearer <access_token>
```

期望：HTTP 200，rsp.success=true。

## 测试 3：错误 access token

```
POST /service/user/get_user_info
Authorization: Bearer eyJBOGUS
```

期望：HTTP 401，error_code=1003 (AUTH_TOKEN_INVALID)。

## 测试 4：缺 Authorization header

```
POST /service/user/get_user_info
（无 Authorization）
```

期望：HTTP 401，error_code=1003。

## 测试 5：Refresh 滚动 + 重放检测

```
POST /service/user/refresh_token   { refresh_token: <old> }
→ 200，返回新 access + 新 refresh
POST /service/user/refresh_token   { refresh_token: <old> }   # 同一 old
→ 200 但 error_code=1008 (AUTH_REFRESH_TOKEN_REUSED)
```

## 测试 6：Logout 后 access 立即失效

```
POST /service/user/logout
Authorization: Bearer <access_token>
→ 200 success
POST /service/user/get_user_info
Authorization: Bearer <same access_token>
→ 401 + error_code=1003
```

## 测试 7：trace_id 透传

```
POST /service/user/get_user_info
Authorization: Bearer <access_token>
X-Trace-Id: aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa
```

期望：响应 header `X-Trace-Id: aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa`；user_server 日志 grep 到该 trace_id。
