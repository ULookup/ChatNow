# Identity 服务补齐设计

> **状态**: 设计完成，待评审
> **日期**: 2026-05-16
> **范围**: Identity 服务从 3/9 RPC → 9/9 RPC，删除旧 UserServiceImpl，proto 清理
> **基线**: `2026-05-14-service-migration-design.md` §3.1、`2026-05-14-p2-jwt-auth.md`、`user/source/user_server.h` 当前 HEAD
> **目标**: 生产可用的 IM Identity 服务，对齐主流 IM 标准

---

## 0. 设计原则

1. **注册即登录** — 注册成功直接返回 JWT token，与微信/Telegram 对齐
2. **密码必须哈希** — 旧代码明文存储必须改为 bcrypt，不向后兼容
3. **零跨服务 RPC 查询** — avatar_url 直接拼接，不调 Media，消除级联故障
4. **双通道就绪** — email 验证码可用，phone 验证码结构就绪暂抛 NOT_IMPLEMENTED
5. **proto 无鉴权字段** — 删所有 `session_id`/`user_id` 残留（横切 spec §5）

---

## 一、RPC 矩阵

| # | RPC | 当前 | 设计要点 |
|---|---|---|---|
| 1 | Login | 部分 (username_pwd ✓) | 补 phone_code 分支（暂抛 NOT_IMPLEMENTED） |
| 2 | Logout | ✅ | 不动 |
| 3 | RefreshToken | ✅ | 不动 |
| 4 | Register | ❌ | `oneof credential`，UsernamePassword 分支完整实现，PhoneVerifyCode 暂抛 |
| 5 | SendVerifyCode | ❌ | `oneof destination`，email→MailClient，phone→NOT_IMPLEMENTED |
| 6 | GetProfile | ❌ | user_id 可选（空=自己），URL 直接拼接，session_id 删 |
| 7 | UpdateProfile | ❌ | 合并旧 4 setter，optional 字段区分，session_id/user_id 删 |
| 8 | GetMultiUserInfo | ❌ | 批量查 DB，URL 直接拼接，不调 RPC |
| 9 | SearchUsers | ❌ | ES 搜索，session_id/user_id 删 |

---

## 二、Proto 变更

### 2.1 identity_service.proto

**RegisterRsp** — 注册即登录，加 AuthTokens + UserInfo：

```protobuf
message RegisterRsp {
    chatnow.common.ResponseHeader header = 1;
    string user_id                 = 2;
    AuthTokens tokens              = 3;
    chatnow.common.UserInfo user_info = 4;
}
```

**SendVerifyCodeReq** — 双通道 `oneof destination`：

```protobuf
message SendVerifyCodeReq {
    string request_id = 1;
    oneof destination {
        string email = 2;
        string phone = 3;
    }
}
```

**GetProfileReq** — 删 `session_id`，保留 optional user_id（空=查自己）：

```protobuf
message GetProfileReq {
    string request_id = 1;
    optional string user_id = 2;
}
```

**UpdateProfileReq** — 删 `user_id` / `session_id`，身份从 metadata 取：

```protobuf
message UpdateProfileReq {
    string request_id = 1;
    optional string nickname       = 2;
    optional string bio            = 3;
    optional string avatar_file_id = 4;
    optional string phone          = 5;
}
```

**SearchUsersReq** — 删 `user_id` / `session_id`：

```protobuf
message SearchUsersReq {
    string request_id = 1;
    string search_key = 2;
}
```

### 2.2 不需要 proto 变更的场景

- `LoginReq` 不变（`oneof credential` + `device_id` + `device_name` 已就绪）
- `LogoutReq` / `RefreshTokenReq` 不变
- `GetMultiUserInfoReq` 不变
- 不需要新增错误码（现有 1001-1009 已覆盖所有认证场景）

---

## 三、RPC 详细设计

### 3.1 Login（补全 phone_code 分支）

当前 P2 实现只处理 `username_pwd`。增加 `phone_code` 分支：

```cpp
void Login(...) {
    // 当前已实现 username_pwd 分支 — 保留不动
    if (request->has_phone_code()) {
        // TODO: 等 SMS SDK 就绪后实现
        throw ServiceError(kNotImplemented, "phone_code login not supported");
    }
    // ... 现有 username_pwd 逻辑不变
}
```

### 3.2 Register

不使用 `HANDLE_RPC`（白名单路由，入站无 metadata）。手写 try/catch + ResponseHeader。

**UsernamePassword 分支：**

```
1. cred = request.username_pwd()
2. nickname_check(cred.nickname) / password_check(cred.password)
   → 不合法抛 kAuthInvalidCredentials
3. UserTable::select_by_nickname(nickname)
   → 已存在抛 kAuthUserAlreadyExists
4. user_id = uuid()
5. password_hash = bcrypt_hashpw(password, &salt)
6. UserTable::insert(user_id, nickname, password_hash, salt)
7. ESUser::append_data(user_id, "", nickname, "", "")
8. 签发 access + refresh token（同 Login）
9. JwtStore::put_active_refresh(...)
10. 设置 UserInfo（user_id, nickname）
11. 返回 user_id + AuthTokens + UserInfo
```

**PhoneVerifyCode 分支：**

```
暂抛 ServiceError(kNotImplemented, "phone_code register not supported")
```

### 3.3 SendVerifyCode

不使用 `HANDLE_RPC`（白名单路由）。

**email 分支：**

```
1. mail = request.email()
2. mail_check(mail) → 不合法抛 kAuthInvalidCredentials
3. code_id = uuid(), code = random_4_digit()
4. MailClient::send(mail, code) → 失败抛 kSystemInternalError
5. Codes::append(code_id, code)
6. 返回 verify_code_id
```

**phone 分支：**

```
暂抛 ServiceError(kNotImplemented, "phone verification not supported")
```

### 3.4 GetProfile

使用 `HANDLE_RPC`（需认证）。

```
1. user_id = request.has_user_id() ? request.user_id() : auth.user_id
2. UserTable::select_by_id(user_id) → 无则抛 kAuthUserNotFound
3. 构造 UserInfo:
   - user_id, nickname, bio, phone
   - avatar_url = media_public_url_prefix + "/" + user.avatar_id()
     （avatar_id 为空时 avatar_url 为空串）
4. 返回 UserInfo
```

### 3.5 UpdateProfile

使用 `HANDLE_RPC`（需认证）。

```
1. UserTable::select_by_id(auth.user_id) → 无则抛 kAuthUserNotFound
2. 逐 optional 字段更新（non-empty 才更新）:
   - nickname: 校验 + user->nickname(v)
   - bio: user->description(v)
   - phone: 校验 E.164 格式 + user->phone(v)
   - avatar_file_id: user->avatar_id(v)
3. UserTable::update(user)
4. ESUser::append_data(user.user_id, user.mail, user.phone,
                        user.nickname, user.description, user.avatar_id)
5. 构造 UserInfo 返回（同 GetProfile）
```

### 3.6 GetMultiUserInfo

使用 `HANDLE_RPC`（需认证）。

```
1. users_id[] = request.users_id()
2. UserTable::select_multi_users(users_id)
3. for each user → 构造 UserInfo（同 GetProfile）
4. 写入 response.users_info map
```

不再调 `FileService_Stub.GetMultiFile` 或 `MediaService_Stub`。avatar_url 直接拼接。

### 3.7 SearchUsers

使用 `HANDLE_RPC`（需认证）。

```
1. keyword = request.search_key()
2. ESUser::search(keyword, size=20) → vector<User>
3. for each user → 构造 UserInfo（同 GetProfile）
4. 写入 response.user_info[]
```

---

## 四、密码安全

### 4.1 bcrypt 选型

- 选 bcrypt 而非 argon2id：项目 C++17，libcrypt 的 `crypt_r` 系统调用或 vendor 一个 bcrypt 小 header 即可，零额外依赖
- cost factor = 10（~100ms/次），对 Login/Register 足够

### 4.2 实现方案

vendor Andrew Moon 的 `bcrypt.h`（MIT license，~200 行，单 header）到 `third/include/bcrypt/bcrypt.h`：

```cpp
// 工具封装到 common/auth/bcrypt_util.hpp
namespace chatnow::auth {
inline std::string hash_password(const std::string& pw) {
    char salt[BCRYPT_HASHSIZE];
    char hash[BCRYPT_HASHSIZE];
    bcrypt_gensalt(10, salt);
    bcrypt_hashpw(pw.c_str(), salt, hash);
    return std::string(hash);
}
inline bool check_password(const std::string& pw, const std::string& hash) {
    return bcrypt_checkpw(pw.c_str(), hash.c_str()) == 0;
}
}
```

### 4.3 Register 写入

```
std::string hash = hash_password(password);
user->password(hash);
// password_salt 留空（bcrypt 自含 salt）
UserTable::insert(user);
```

### 4.4 Login 比对

```
// 旧：user->password() != cred.password()
// 新：
if (!check_password(cred.password(), user->password()))
    throw ServiceError(kAuthInvalidCredentials, "bad credentials");
```

### 4.5 旧数据问题

开发阶段可以 `docker-compose down -v` 重建 DB，无需兼容旧哈希。强制全量 bcrypt。

---

## 五、avatar_url 拼接

### 5.1 策略

`{media_public_url_prefix}/{avatar_id}`

不调 Media RPC，理由：
- `chatnow-media-public` bucket 是 public-read（横切 spec §1 总览图）
- 头像文件 ID 写入后不变（只 append 不 update）
- CDN 承担流量，消除 Media 服务故障对 Identity 的级联影响
- GetMultiUserInfo 批量查 N 个用户只需 1 次 DB 查询，零 RPC 开销

### 5.2 注入方式

Builder 加 gflag `--media_public_url_prefix`，构造时注入 IdentityServiceImpl：

```cpp
void make_media_config(const std::string& public_url_prefix) {
    _media_public_url_prefix = public_url_prefix;
    // 确保不以 / 结尾
    while (!_media_public_url_prefix.empty() && _media_public_url_prefix.back() == '/')
        _media_public_url_prefix.pop_back();
}
```

---

## 六、Builder / Server 重构

### 6.1 重命名

```
UserServer          → IdentityServer
UserServerBuilder   → IdentityServerBuilder
UserServiceImpl     → 删除（整个类）
```

### 6.2 依赖精简

| 组件 | 旧（UserServiceImpl） | 新（IdentityServiceImpl） |
|---|---|---|
| ESUser | ✅ | ✅ |
| UserTable | ✅ | ✅ |
| Codes (Redis) | ✅ | ✅ — 验证码 |
| Session (Redis) | ✅ | ❌ 移除 — 已被 JWT 替代 |
| Status (Redis) | ✅ | ❌ 移除 — 已被 JWT+JwtStore 替代 |
| MailClient | ✅ | ✅ |
| JwtCodec | ❌ (旧类无) | ✅ |
| JwtStore | ❌ (旧类无) | ✅ |
| FileService_Stub | ✅ | ❌ 移除 |
| MediaService_Stub | ❌ | ❌ 不移 — avatar URL 直接拼接 |
| ServiceManager | ✅ | ❌ 移除 — 不调跨服务 RPC |

### 6.3 IdentityServerBuilder 构造函数注入

```cpp
class IdentityServerBuilder {
    // make_mysql_object / make_redis_object / make_es_object / make_mail_object
    // make_jwt_object (已有)
    // make_media_config(public_url_prefix)  ← 新增
    // make_discovery_object / make_registry_object
    // make_rpc_object → 只注册 IdentityServiceImpl，不注册旧 UserServiceImpl
    // build → 返回 IdentityServer::ptr
};
```

### 6.4 IdentityServer

```cpp
class IdentityServer {
    // 接口同 UserServer：start() 调 RunUntilAskedToQuit()
};
```

---

## 七、文件清单

| 文件 | 动作 | 说明 |
|---|---|---|
| `proto/identity/identity_service.proto` | 修改 | RegisterRsp 加字段；SendVerifyCodeReq 改 oneof；删 session_id/user_id |
| `user/source/user_server.h` | 重写 | 删 UserServiceImpl，IdentityServiceImpl 9 RPC 完整实现，Builder/Server 改名 |
| `user/source/user_server.cc` | 修改 | main 中类名同步 |
| `conf/user_server.conf` | 修改 | gflag 加 `--media_public_url_prefix` |
| `third/include/bcrypt/bcrypt.h` | 新增 | vendor bcrypt 单 header |
| `common/auth/bcrypt_util.hpp` | 新增 | hash_password / check_password 封装 |
| `common/dao/mysql_user.hpp` | 可能改 | 如 insert 需要接收 password_salt 参数 |

### 不需要修改的文件

- `common/auth/jwt_codec.hpp` / `jwt_store.hpp` / `auth_context.hpp` / `auth_config_loader.hpp` — 已就绪
- `common/error/error_codes.hpp` — 现有错误码已覆盖所有场景
- `common/dao/data_es.hpp` — ESUser 已支持 phone/status
- `common/dao/data_redis.hpp` — Codes 类已就绪
- `user/CMakeLists.txt` — 链接库不变

---

## 八、RPC 鉴权模式

| RPC | 使用 HANDLE_RPC | 原因 |
|---|---|---|
| Login | 否 | 白名单路由，无 metadata |
| Logout | 否 | 手写 try/catch（auth_context 从 controller 取） |
| RefreshToken | 否 | 白名单路由，自验证 refresh token |
| Register | 否 | 白名单路由，无 metadata |
| SendVerifyCode | 否 | 白名单路由，无 metadata |
| GetProfile | 是 | 需认证 |
| UpdateProfile | 是 | 需认证 |
| GetMultiUserInfo | 是 | 需认证 |
| SearchUsers | 是 | 需认证 |

---

## 九、与后续服务的接口契约

| 消费方 | 调用的 RPC | 说明 |
|---|---|---|
| Relationship | GetMultiUserInfo | stub 类名已是 IdentityService_Stub |
| Conversation | GetMultiUserInfo | 同上 |
| Message | GetMultiUserInfo | 同上（如还在用） |
| Transmite | GetProfile | 改用新包名 `chatnow.identity.GetProfileReq/Rsp`，`IdentityService_Stub` |
| Gateway | 全部 9 RPC | 鉴权模式更新（见 §八） |

---

## 十、不做的事

- ❌ 短信 SDK 集成（等 SMS 供应商确定后单开 task）
- ❌ 密码重置 / 邮箱找回（YAGNI — 本期无客户端场景）
- ❌ 注销账号（`UserStatus::DEACTIVE` 字段已预留，但业务逻辑不是本期范围）
- ❌ 设备管理 / 多端登录策略（横切 spec 标注 P3）
- ❌ JWT kid 轮换（已有 `auth_config_loader` 和密钥轮换 runbook，不属本 spec）
- ❌ Profile 隐私设置（好友可见/公开/私密 — YAGNI）
