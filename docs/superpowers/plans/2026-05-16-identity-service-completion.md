# Identity 服务补齐 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Identity 服务从 3/9 RPC 补全到 9/9 RPC，删除旧 UserServiceImpl，proto 清理鉴权残留，bcrypt 替代明文密码。

**Architecture:** 扩展现有 IdentityServiceImpl（已有 Login/Logout/RefreshToken），新增 Register/SendVerifyCode/GetProfile/UpdateProfile/GetMultiUserInfo/SearchUsers 6 个 RPC。Register 注册即登录（直接返回 JWT）。头像 URL 直接拼接，不调跨服务 RPC。验证码双通道 oneof（email 就绪 / phone 暂抛 NOT_IMPLEMENTED）。删除旧 UserServiceImpl 后将 Builder 重命名为 IdentityServerBuilder。

**Tech Stack:** C++17, brpc, protobuf, ODB+MySQL, Elasticsearch (elasticlient), Redis (redis-plus-plus), libcurl SMTP (MailClient), bcrypt (vendor header), JwtCodec/JwtStore (已有), spdlog.

**Spec:** `docs/superpowers/specs/2026-05-16-identity-service-completion-design.md`

---

## File Structure

| 文件 | 动作 | 职责 |
|---|---|---|
| `third/include/bcrypt/bcrypt.h` | 新增 | vendor bcrypt 单 header（MIT license） |
| `common/auth/bcrypt_util.hpp` | 新增 | `hash_password` / `check_password` 封装 |
| `common/error/error_codes.hpp` | 修改 | 加 `kNotImplemented = 9005` |
| `proto/identity/identity_service.proto` | 修改 | RegisterRsp 加字段；SendVerifyCodeReq 改 oneof；删 3 处 session_id/user_id |
| `user/source/user_server.h` | 重写 | 9 RPC IdentityServiceImpl + IdentityServerBuilder + IdentityServer |
| `user/source/user_server.cc` | 修改 | main 类名同步 + 加 media_public_url_prefix |
| `conf/user_server.conf` | 修改 | 加 `--media_public_url_prefix` |

---

## Task 1: 加 `kNotImplemented` 错误码

**Files:**
- Modify: `common/error/error_codes.hpp`

- [ ] **Step 1: 在 `kSystemInvalidArgument` 后加一行**

```cpp
inline constexpr int32_t kNotImplemented           = 9005;
```

- [ ] **Step 2: Commit**

```bash
git add common/error/error_codes.hpp
git commit -m "error: add kNotImplemented 9005 for unimplemented RPC branches"
```

---

## Task 2: Vendor bcrypt + 创建 `bcrypt_util.hpp`

**Files:**
- Create: `third/include/bcrypt/bcrypt.h`
- Create: `common/auth/bcrypt_util.hpp`

### bcrypt.h

vendor Andrew Moon's `bcrypt.h` (MIT license, ~200 lines). 由于是外部 header，用以下命令获取：

```bash
curl -sL https://raw.githubusercontent.com/andrew-moon/bcrypt/refs/tags/v1.4/main/bcrypt.h -o third/include/bcrypt/bcrypt.h
```

如网络不可达，用本地等效实现写入。

- [ ] **Step 1: 创建目录 + vendor bcrypt.h**

```bash
mkdir -p third/include/bcrypt
curl -sL https://raw.githubusercontent.com/andrew-moon/bcrypt/refs/tags/v1.4/main/bcrypt.h -o third/include/bcrypt/bcrypt.h
wc -l third/include/bcrypt/bcrypt.h
```

Expected: ~330 lines

- [ ] **Step 2: 创建 `common/auth/bcrypt_util.hpp`**

```cpp
#pragma once

#include "bcrypt/bcrypt.h"
#include <string>
#include <stdexcept>

namespace chatnow::auth {

inline std::string hash_password(const std::string& pw) {
    if (pw.empty()) {
        throw std::invalid_argument("password must not be empty");
    }
    char salt[BCRYPT_HASHSIZE];
    char hash[BCRYPT_HASHSIZE];
    if (bcrypt_gensalt(10, salt) != 0) {
        throw std::runtime_error("bcrypt_gensalt failed");
    }
    if (bcrypt_hashpw(pw.c_str(), salt, hash) != 0) {
        throw std::runtime_error("bcrypt_hashpw failed");
    }
    return std::string(hash);
}

inline bool check_password(const std::string& pw, const std::string& hash) {
    if (pw.empty() || hash.empty()) return false;
    return bcrypt_checkpw(pw.c_str(), hash.c_str()) == 0;
}

}  // namespace chatnow::auth
```

- [ ] **Step 3: Commit**

```bash
git add third/include/bcrypt/bcrypt.h common/auth/bcrypt_util.hpp
git commit -m "auth: vendor bcrypt.h + bcrypt_util.hpp（hash_password / check_password）"
```

---

## Task 3: Proto 清理

**Files:**
- Modify: `proto/identity/identity_service.proto`

读写整个文件。

- [ ] **Step 1: 读当前文件确认起点**

```bash
cat proto/identity/identity_service.proto
```

- [ ] **Step 2: 3 处修改**

**修改 1 — RegisterRsp 加 tokens + user_info（line 53-56）：**

旧：
```protobuf
message RegisterRsp {
    chatnow.common.ResponseHeader header = 1;
    string user_id        = 2;
}
```

新：
```protobuf
message RegisterRsp {
    chatnow.common.ResponseHeader header = 1;
    string user_id                 = 2;
    AuthTokens tokens              = 3;
    chatnow.common.UserInfo user_info = 4;
}
```

**修改 2 — SendVerifyCodeReq 改为 oneof（line 89-96）：**

旧：
```protobuf
message SendVerifyCodeReq {
    string request_id = 1;
    string phone      = 2;
}
```

新：
```protobuf
message SendVerifyCodeReq {
    string request_id = 1;
    oneof destination {
        string email = 2;
        string phone = 3;
    }
}
```

**修改 3 — 删 3 处 session_id/user_id：**

GetProfileReq (line 112-119) — 删 `optional string session_id = 3;`：

```protobuf
message GetProfileReq {
    string request_id = 1;
    optional string user_id = 2;
}
```

UpdateProfileReq (line 121-132) — 删 `optional string user_id = 2; optional string session_id = 3;`，重新编号：

```protobuf
message UpdateProfileReq {
    string request_id = 1;
    optional string nickname       = 2;
    optional string bio            = 3;
    optional string avatar_file_id = 4;
    optional string phone          = 5;
}
```

SearchUsersReq (line 138-143) — 删 `optional string session_id = 3; optional string user_id = 4;`：

```protobuf
message SearchUsersReq {
    string request_id = 1;
    string search_key = 2;
}
```

- [ ] **Step 3: 验证 RegisterRsp 能引用 AuthTokens**

`AuthTokens` 在同文件 line 23。确认它在 `RegisterRsp` 之前定义（它是）。如顺序不对则调整。

- [ ] **Step 4: Commit**

```bash
git add proto/identity/identity_service.proto
git commit -m "proto(identity): RegisterRsp 加 token+user_info；SendVerifyCode 改 oneof；删 session_id/user_id 残留"
```

---

## Task 4: 重写 user_server.h — IdentityServiceImpl 构造 + Login 补全

**Files:**
- Modify: `user/source/user_server.h`

本任务只改 IdentityServiceImpl 的构造函数、成员变量、以及 Login 方法加 phone_code 分支。其余 RPC 在 Task 5–10 逐个加。

- [ ] **Step 1: 替换 IdentityServiceImpl 构造函数和成员变量**

**旧（line 37-43, 226-229）：**

```cpp
    IdentityServiceImpl(const std::shared_ptr<odb::core::database> &mysql_client,
                        const std::shared_ptr<auth::JwtCodec> &jwt_codec,
                        const std::shared_ptr<auth::JwtStore> &jwt_store)
        : _mysql_user(std::make_shared<UserTable>(mysql_client)),
          _jwt_codec(jwt_codec),
          _jwt_store(jwt_store) {}

    ~IdentityServiceImpl() override = default;
```

**新：**

```cpp
    IdentityServiceImpl(const std::shared_ptr<odb::core::database> &mysql_client,
                        const std::shared_ptr<elasticlient::Client> &es_client,
                        const std::shared_ptr<sw::redis::Redis> &redis_client,
                        const std::shared_ptr<MailClient> &mail_client,
                        const std::shared_ptr<auth::JwtCodec> &jwt_codec,
                        const std::shared_ptr<auth::JwtStore> &jwt_store,
                        const std::string &media_public_url_prefix)
        : _mysql_user(std::make_shared<UserTable>(mysql_client)),
          _es_user(std::make_shared<ESUser>(es_client)),
          _redis_codes(std::make_shared<Codes>(redis_client)),
          _mail_client(mail_client),
          _jwt_codec(jwt_codec),
          _jwt_store(jwt_store),
          _media_public_url_prefix(media_public_url_prefix)
    {
        _es_user->create_index();
    }

    ~IdentityServiceImpl() override = default;
```

**成员变量 — 旧（line 226-229）：**

```cpp
private:
    std::shared_ptr<UserTable>          _mysql_user;
    std::shared_ptr<auth::JwtCodec>     _jwt_codec;
    std::shared_ptr<auth::JwtStore>     _jwt_store;
```

**成员变量 — 新：**

```cpp
private:
    std::shared_ptr<UserTable>          _mysql_user;
    std::shared_ptr<ESUser>             _es_user;
    std::shared_ptr<Codes>              _redis_codes;
    std::shared_ptr<MailClient>         _mail_client;
    std::shared_ptr<auth::JwtCodec>     _jwt_codec;
    std::shared_ptr<auth::JwtStore>     _jwt_store;
    std::string                         _media_public_url_prefix;

    // ---- 输入校验 ----
    static bool nickname_check(const std::string &nickname) {
        if (nickname.size() < 3 || nickname.size() > 22) return false;
        for (char c : nickname) {
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '_' || c == '-'))
                return false;
        }
        return true;
    }

    static bool password_check(const std::string &password) {
        if (password.size() < 6 || password.size() > 15) return false;
        for (char c : password) {
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '_' || c == '-'))
                return false;
        }
        return true;
    }

    static bool mail_check(const std::string &mail) {
        auto at = mail.find('@');
        auto dot = mail.rfind('.');
        auto sp = mail.find(' ');
        return at != std::string::npos && dot != std::string::npos && sp == std::string::npos;
    }

    // ---- 工具 ----
    std::string uuid() { return utils::uuid(); }

    std::string make_avatar_url(const std::string &avatar_id) {
        if (avatar_id.empty() || _media_public_url_prefix.empty()) return "";
        return _media_public_url_prefix + "/" + avatar_id;
    }

    // ---- UserInfo 组装 ----
    void fill_user_info(::chatnow::common::UserInfo *u, const User &user) {
        u->set_user_id(user.user_id());
        u->set_nickname(user.nickname());
        if (user.description_present()) u->set_bio(user.description());
        if (user.phone_present()) u->set_phone(user.phone());
        u->set_avatar_url(make_avatar_url(user.avatar_id()));
    }
```

- [ ] **Step 2: Login 加 phone_code 分支**

在现有 Login 方法内，`try` 块中 `if (!request->has_username_pwd())` 之前，加一个 phone_code 检查：

```cpp
        try {
            // phone_code 分支：结构就绪，等 SMS SDK
            if (request->has_phone_code()) {
                throw ServiceError(::chatnow::error::kNotImplemented,
                                   "phone_code login not yet supported");
            }
            // 1. 凭据校验（P2 仅支持 username_pwd）
            if (!request->has_username_pwd()) {
                throw ServiceError(::chatnow::error::kAuthInvalidCredentials,
                                   "no valid credential provided");
            }
            // ... 以下现有 username_pwd 逻辑不变
```

同时把 Login 中的密码比对从明文改为 bcrypt：

**旧（line 64）：**
```cpp
            if (!user || user->password() != cred.password()) {
```

**新：**
```cpp
            if (!user || !auth::check_password(cred.password(), user->password())) {
```

- [ ] **Step 3: 在文件头部加新 include**

在 `#include "auth/jwt_store.hpp"` 之后加：

```cpp
#include "auth/bcrypt_util.hpp"
```

- [ ] **Step 4: Commit**

```bash
git add user/source/user_server.h
git commit -m "identity: 扩展构造参数 + Login 加 phone_code 分支 + bcrypt 比对"
```

---

## Task 5: IdentityServiceImpl 加 Register RPC

**Files:**
- Modify: `user/source/user_server.h`

在 `RefreshToken` 方法之后、`private:` 之前插入 Register 方法。

- [ ] **Step 1: 在 IdentityServiceImpl 中加 Register**

```cpp
    void Register(::google::protobuf::RpcController* controller,
                  const ::chatnow::identity::RegisterReq* request,
                  ::chatnow::identity::RegisterRsp* response,
                  ::google::protobuf::Closure* done) override
    {
        brpc::ClosureGuard rpc_guard(done);
        auto* header = response->mutable_header();
        header->set_request_id(request->request_id());
        try {
            std::string user_id;
            std::string nickname;
            std::string phone;

            if (request->has_username_pwd()) {
                const auto& cred = request->username_pwd();
                nickname = request->nickname();
                // 校验
                if (!nickname_check(nickname)) {
                    throw ServiceError(::chatnow::error::kAuthInvalidCredentials,
                                       "nickname invalid");
                }
                if (!password_check(cred.password())) {
                    throw ServiceError(::chatnow::error::kAuthInvalidCredentials,
                                       "password invalid");
                }
                // 重名检查
                auto existing = _mysql_user->select_by_nickname(nickname);
                if (existing) {
                    throw ServiceError(::chatnow::error::kAuthUserAlreadyExists,
                                       "nickname already taken");
                }
                // 创建用户
                user_id = uuid();
                std::string hash = auth::hash_password(cred.password());
                auto user = std::make_shared<User>(user_id, nickname, hash);
                if (!_mysql_user->insert(user)) {
                    throw ServiceError(::chatnow::error::kSystemInternalError,
                                       "db insert failed");
                }
                // ES 索引
                _es_user->append_data(user_id, "", phone, nickname, "", "");
            } else if (request->has_phone_code()) {
                throw ServiceError(::chatnow::error::kNotImplemented,
                                   "phone_code register not yet supported");
            } else {
                throw ServiceError(::chatnow::error::kAuthInvalidCredentials,
                                   "no valid credential provided");
            }

            // 注册即登录：签发 JWT
            std::string device_id = "default_device";
            std::string access_jti  = auth::JwtCodec::random_jti();
            std::string refresh_jti = auth::JwtCodec::random_jti();
            std::string access_tok  = _jwt_codec->sign_access(user_id, device_id, access_jti);
            std::string refresh_tok = _jwt_codec->sign_refresh(user_id, device_id, refresh_jti);
            _jwt_store->put_active_refresh(user_id, device_id,
                                           refresh_jti, _jwt_codec->refresh_ttl_sec());

            auto* tokens = response->mutable_tokens();
            tokens->set_access_token(access_tok);
            tokens->set_refresh_token(refresh_tok);
            tokens->set_access_expires_in_sec(_jwt_codec->access_ttl_sec());
            tokens->set_refresh_expires_in_sec(_jwt_codec->refresh_ttl_sec());

            auto* uinfo = response->mutable_user_info();
            uinfo->set_user_id(user_id);
            uinfo->set_nickname(nickname);

            response->set_user_id(user_id);
            header->set_success(true);
            header->set_error_code(::chatnow::common::OK);
            LOG_INFO("Register OK rid={} uid={}", request->request_id(), user_id);
        } catch (const ServiceError& e) {
            header->set_success(false);
            header->set_error_code(e.code());
            header->set_error_message(e.message());
            LOG_WARN("Register 失败 rid={} code={} msg={}",
                     request->request_id(), e.code(), e.message());
        } catch (const std::exception& e) {
            header->set_success(false);
            header->set_error_code(::chatnow::error::kSystemInternalError);
            header->set_error_message("internal error");
            LOG_ERROR("Register 异常 rid={}: {}", request->request_id(), e.what());
        }
    }
```

- [ ] **Step 2: Commit**

```bash
git add user/source/user_server.h
git commit -m "identity: 实现 Register（username_pwd + 注册即登录签发 JWT）"
```

---

## Task 6: IdentityServiceImpl 加 SendVerifyCode RPC

**Files:**
- Modify: `user/source/user_server.h`

- [ ] **Step 1: 在 IdentityServiceImpl 中加 SendVerifyCode**

在 Register 方法之后插入：

```cpp
    void SendVerifyCode(::google::protobuf::RpcController* controller,
                        const ::chatnow::identity::SendVerifyCodeReq* request,
                        ::chatnow::identity::SendVerifyCodeRsp* response,
                        ::google::protobuf::Closure* done) override
    {
        brpc::ClosureGuard rpc_guard(done);
        auto* header = response->mutable_header();
        header->set_request_id(request->request_id());
        try {
            if (request->has_email()) {
                std::string mail = request->email();
                if (!mail_check(mail)) {
                    throw ServiceError(::chatnow::error::kAuthInvalidCredentials,
                                       "email format invalid");
                }
                std::string code_id = uuid();
                std::string code = utils::verifyCode();
                if (!_mail_client->send(mail, code)) {
                    throw ServiceError(::chatnow::error::kSystemInternalError,
                                       "send email failed");
                }
                _redis_codes->append(code_id, code);
                response->set_verify_code_id(code_id);
            } else if (request->has_phone()) {
                throw ServiceError(::chatnow::error::kNotImplemented,
                                   "phone verification not yet supported");
            } else {
                throw ServiceError(::chatnow::error::kAuthInvalidCredentials,
                                   "no destination specified");
            }
            header->set_success(true);
            header->set_error_code(::chatnow::common::OK);
            LOG_INFO("SendVerifyCode OK rid={}", request->request_id());
        } catch (const ServiceError& e) {
            header->set_success(false);
            header->set_error_code(e.code());
            header->set_error_message(e.message());
            LOG_WARN("SendVerifyCode 失败 rid={} code={}",
                     request->request_id(), e.code());
        } catch (const std::exception& e) {
            header->set_success(false);
            header->set_error_code(::chatnow::error::kSystemInternalError);
            header->set_error_message("internal error");
            LOG_ERROR("SendVerifyCode 异常 rid={}: {}", request->request_id(), e.what());
        }
    }
```

- [ ] **Step 2: 确认 `utils::verifyCode()` 存在**

```bash
grep -n "verifyCode" utils/utils.hpp | head -3
```

Expected: 有定义（旧 UserServiceImpl 中用过 `verifyCode()`）。

- [ ] **Step 3: Commit**

```bash
git add user/source/user_server.h
git commit -m "identity: 实现 SendVerifyCode（email 就绪 + phone 暂抛 NOT_IMPLEMENTED）"
```

---

## Task 7: IdentityServiceImpl 加 GetProfile RPC

**Files:**
- Modify: `user/source/user_server.h`

- [ ] **Step 1: 在 IdentityServiceImpl 中加 GetProfile**

使用 `HANDLE_RPC`（需认证，从 metadata 取 user_id）：

```cpp
    void GetProfile(::google::protobuf::RpcController* controller,
                    const ::chatnow::identity::GetProfileReq* request,
                    ::chatnow::identity::GetProfileRsp* response,
                    ::google::protobuf::Closure* done) override
    {
        brpc::ClosureGuard rpc_guard(done);
        auto* cntl = static_cast<brpc::Controller*>(controller);
        HANDLE_RPC(cntl, request, response, {
            std::string uid = request->has_user_id() ? request->user_id() : auth.user_id;
            auto user = _mysql_user->select_by_id(uid);
            if (!user) {
                throw ServiceError(::chatnow::error::kAuthUserNotFound,
                                   "user not found: " + uid);
            }
            fill_user_info(response->mutable_user_info(), *user);
        });
    }
```

- [ ] **Step 2: Commit**

```bash
git add user/source/user_server.h
git commit -m "identity: 实现 GetProfile（metadata 取身份 + avatar URL 直接拼接）"
```

---

## Task 8: IdentityServiceImpl 加 UpdateProfile RPC

**Files:**
- Modify: `user/source/user_server.h`

- [ ] **Step 1: 在 IdentityServiceImpl 中加 UpdateProfile**

```cpp
    void UpdateProfile(::google::protobuf::RpcController* controller,
                       const ::chatnow::identity::UpdateProfileReq* request,
                       ::chatnow::identity::UpdateProfileRsp* response,
                       ::google::protobuf::Closure* done) override
    {
        brpc::ClosureGuard rpc_guard(done);
        auto* cntl = static_cast<brpc::Controller*>(controller);
        HANDLE_RPC(cntl, request, response, {
            auto user = _mysql_user->select_by_id(auth.user_id);
            if (!user) {
                throw ServiceError(::chatnow::error::kAuthUserNotFound,
                                   "user not found");
            }
            if (request->has_nickname()) {
                if (!nickname_check(request->nickname())) {
                    throw ServiceError(::chatnow::error::kAuthInvalidCredentials,
                                       "nickname invalid");
                }
                user->nickname(request->nickname());
            }
            if (request->has_bio()) {
                user->description(request->bio());
            }
            if (request->has_avatar_file_id()) {
                user->avatar_id(request->avatar_file_id());
            }
            if (request->has_phone()) {
                user->phone(request->phone());
            }
            if (!_mysql_user->update(user)) {
                throw ServiceError(::chatnow::error::kSystemInternalError,
                                   "db update failed");
            }
            _es_user->append_data(user->user_id(), user->mail(), user->phone(),
                                  user->nickname(), user->description(),
                                  user->avatar_id());
            fill_user_info(response->mutable_user_info(), *user);
        });
    }
```

- [ ] **Step 2: Commit**

```bash
git add user/source/user_server.h
git commit -m "identity: 实现 UpdateProfile（合并 4 setter -> optional 字段）"
```

---

## Task 9: IdentityServiceImpl 加 GetMultiUserInfo RPC

**Files:**
- Modify: `user/source/user_server.h`

- [ ] **Step 1: 在 IdentityServiceImpl 中加 GetMultiUserInfo**

```cpp
    void GetMultiUserInfo(::google::protobuf::RpcController* controller,
                          const ::chatnow::identity::GetMultiUserInfoReq* request,
                          ::chatnow::identity::GetMultiUserInfoRsp* response,
                          ::google::protobuf::Closure* done) override
    {
        brpc::ClosureGuard rpc_guard(done);
        auto* cntl = static_cast<brpc::Controller*>(controller);
        HANDLE_RPC(cntl, request, response, {
            std::vector<std::string> id_list;
            for (int i = 0; i < request->users_id_size(); ++i) {
                id_list.push_back(request->users_id(i));
            }
            auto users = _mysql_user->select_multi_users(id_list);
            auto* user_map = response->mutable_users_info();
            for (auto& u : users) {
                ::chatnow::common::UserInfo ui;
                fill_user_info(&ui, u);
                (*user_map)[ui.user_id()] = ui;
            }
        });
    }
```

- [ ] **Step 2: Commit**

```bash
git add user/source/user_server.h
git commit -m "identity: 实现 GetMultiUserInfo（批量 DB 查询 + URL 直接拼接）"
```

---

## Task 10: IdentityServiceImpl 加 SearchUsers RPC

**Files:**
- Modify: `user/source/user_server.h`

- [ ] **Step 1: 在 IdentityServiceImpl 中加 SearchUsers**

```cpp
    void SearchUsers(::google::protobuf::RpcController* controller,
                     const ::chatnow::identity::SearchUsersReq* request,
                     ::chatnow::identity::SearchUsersRsp* response,
                     ::google::protobuf::Closure* done) override
    {
        brpc::ClosureGuard rpc_guard(done);
        auto* cntl = static_cast<brpc::Controller*>(controller);
        HANDLE_RPC(cntl, request, response, {
            auto users = _es_user->search(request->search_key(), 20);
            for (auto& u : users) {
                fill_user_info(response->add_user_info(), u);
            }
        });
    }
```

- [ ] **Step 2: Commit**

```bash
git add user/source/user_server.h
git commit -m "identity: 实现 SearchUsers（ES 搜索）"
```

---

## Task 11: 删除旧 UserServiceImpl + 重命名 Builder / Server

**Files:**
- Modify: `user/source/user_server.h`

本任务删除整个 UserServiceImpl 类（~615 行），将 `UserServer` → `IdentityServer`，`UserServerBuilder` → `IdentityServerBuilder`，并更新 Builder 的 `make_rpc_object`。

- [ ] **Step 1: 删除 UserServiceImpl 类**

删除 line 233-847 的整个 `class UserServiceImpl : public UserService { ... };`。

- [ ] **Step 2: 重命名 UserServer → IdentityServer**

**旧（line 849-878）：**

```cpp
class UserServer
{
public:
    using ptr = std::shared_ptr<UserServer>;

    UserServer(...) { ... }
    ~UserServer() = default;
    void start() { _rpc_server->RunUntilAskedToQuit(); }
private:
    ...
};
```

**新：**

```cpp
class IdentityServer
{
public:
    using ptr = std::shared_ptr<IdentityServer>;

    IdentityServer(const Discovery::ptr &service_discover,
                   const Registry::ptr &reg_client,
                   const std::shared_ptr<elasticlient::Client> &es_client,
                   const std::shared_ptr<odb::core::database> &mysql_client,
                   const std::shared_ptr<sw::redis::Redis> &redis_client,
                   const std::shared_ptr<brpc::Server> &server)
        : _service_discover(service_discover),
          _reg_client(reg_client),
          _es_client(es_client),
          _mysql_client(mysql_client),
          _redis_client(redis_client),
          _rpc_server(server) {}
    ~IdentityServer() = default;
    void start() { _rpc_server->RunUntilAskedToQuit(); }

private:
    Discovery::ptr _service_discover;
    Registry::ptr _reg_client;
    std::shared_ptr<brpc::Server> _rpc_server;
    std::shared_ptr<elasticlient::Client> _es_client;
    std::shared_ptr<odb::core::database> _mysql_client;
    std::shared_ptr<sw::redis::Redis> _redis_client;
};
```

- [ ] **Step 3: 重命名 UserServerBuilder → IdentityServerBuilder**

**类名和字段改名：**

```cpp
class IdentityServerBuilder
{
public:
    void make_es_object(const std::vector<std::string> host_list) {
        _es_client = ESClientFactory::create(host_list);
    }
    void make_mysql_object(const std::string &user, const std::string &password,
                           const std::string &host, const std::string &db,
                           const std::string &cset, uint16_t port, int conn_pool_count) {
        _mysql_client = ODBFactory::create(user, password, host, db, cset, port, conn_pool_count);
    }
    void make_redis_object(const std::string &host, uint16_t port, int db, bool keep_alive) {
        _redis_client = RedisClientFactory::create(host, port, db, keep_alive);
    }
    void make_jwt_object(const std::string &auth_config_path) {
        if (!_redis_client) { LOG_ERROR("make_jwt_object 必须在 make_redis_object 之后"); abort(); }
        auto cfg = ::chatnow::auth::load_jwt_config_from_file(auth_config_path);
        _jwt_codec = std::make_shared<::chatnow::auth::JwtCodec>(std::move(cfg));
        _jwt_store = std::make_shared<::chatnow::auth::JwtStore>(_redis_client);
    }
    void make_mail_object(const std::string &mail_username, const std::string &mail_password,
                          const std::string &mail_url, const std::string &mail_from) {
        mail_settings settings = { mail_username, mail_password, mail_url, mail_from };
        _mail_client = std::make_shared<MailClient>(settings);
    }
    void make_media_config(const std::string &public_url_prefix) {
        _media_public_url_prefix = public_url_prefix;
        while (!_media_public_url_prefix.empty() && _media_public_url_prefix.back() == '/')
            _media_public_url_prefix.pop_back();
    }
    void make_discovery_object(const std::string &reg_host,
                               const std::string &base_service_name) {
        _mm_channels = std::make_shared<ServiceManager>();
        auto put_cb = std::bind(&ServiceManager::onServiceOnline, _mm_channels.get(),
                                std::placeholders::_1, std::placeholders::_2);
        auto del_cb = std::bind(&ServiceManager::onServiceOffline, _mm_channels.get(),
                                std::placeholders::_1, std::placeholders::_2);
        _service_discover = std::make_shared<Discovery>(reg_host, base_service_name, put_cb, del_cb);
    }
    void make_registry_object(const std::string &reg_host,
                              const std::string &service_name,
                              const std::string &access_host) {
        _reg_client = std::make_shared<Registry>(reg_host);
        _reg_client->registry(service_name, access_host);
    }
    void make_rpc_object(uint16_t port, uint32_t timeout, uint8_t num_threads) {
        _rpc_server = std::make_shared<brpc::Server>();
        if (!_es_client) { LOG_ERROR("还未初始化ES"); abort(); }
        if (!_mysql_client) { LOG_ERROR("还未初始化MySQL"); abort(); }
        if (!_redis_client) { LOG_ERROR("还未初始化Redis"); abort(); }
        if (!_mail_client) { LOG_ERROR("还未初始化MailClient"); abort(); }
        if (!_jwt_codec || !_jwt_store) { LOG_ERROR("还未初始化JWT"); abort(); }

        IdentityServiceImpl *service = new IdentityServiceImpl(
            _mysql_client, _es_client, _redis_client, _mail_client,
            _jwt_codec, _jwt_store, _media_public_url_prefix);
        int ret = _rpc_server->AddService(service, brpc::ServiceOwnership::SERVER_OWNS_SERVICE);
        if (ret == -1) { LOG_ERROR("添加IdentityService失败!"); abort(); }

        brpc::ServerOptions options;
        options.idle_timeout_sec = timeout;
        options.num_threads = num_threads;
        ret = _rpc_server->Start(port, &options);
        if (ret == -1) { LOG_ERROR("服务启动失败!"); abort(); }
    }
    IdentityServer::ptr build() {
        if (!_service_discover) { LOG_ERROR("还未初始化服务发现"); abort(); }
        if (!_reg_client) { LOG_ERROR("还未初始化服务注册"); abort(); }
        if (!_rpc_server) { LOG_ERROR("还未初始化RPC服务器"); abort(); }
        return std::make_shared<IdentityServer>(_service_discover, _reg_client,
            _es_client, _mysql_client, _redis_client, _rpc_server);
    }

private:
    Registry::ptr _reg_client;
    std::shared_ptr<elasticlient::Client> _es_client;
    std::shared_ptr<odb::core::database> _mysql_client;
    std::shared_ptr<sw::redis::Redis> _redis_client;
    std::shared_ptr<MailClient> _mail_client;
    ServiceManager::ptr _mm_channels;
    Discovery::ptr _service_discover;
    std::shared_ptr<::chatnow::auth::JwtCodec> _jwt_codec;
    std::shared_ptr<::chatnow::auth::JwtStore> _jwt_store;
    std::string _media_public_url_prefix;
    std::shared_ptr<brpc::Server> _rpc_server;
};
```

- [ ] **Step 4: Commit**

```bash
git add user/source/user_server.h
git commit -m "identity: 删除 UserServiceImpl + 重命名为 IdentityServerBuilder"
```

---

## Task 12: 更新 user_server.cc (main)

**Files:**
- Modify: `user/source/user_server.cc`

- [ ] **Step 1: 同步类名 + 加 media_public_url_prefix gflag**

**加 flag（在 `DEFINE_string(auth_config, ...)` 之后）：**

```cpp
DEFINE_string(media_public_url_prefix, "https://cdn.chatnow.com/public", "Media 公开 bucket URL 前缀");
```

**改 main 函数中的类名引用（line 45, 51, 52）：**

```cpp
    chatnow::IdentityServerBuilder isb;
    isb.make_es_object({FLAGS_es_host});
    isb.make_mysql_object(FLAGS_mysql_user, FLAGS_mysql_pswd, FLAGS_mysql_host,
                          FLAGS_mysql_db, FLAGS_mysql_cset, FLAGS_mysql_port,
                          FLAGS_mysql_pool_count);
    isb.make_redis_object(FLAGS_redis_host, FLAGS_redis_port, FLAGS_redis_db,
                          FLAGS_redis_keep_alive);
    isb.make_jwt_object(FLAGS_auth_config);
    isb.make_mail_object(FLAGS_mail_user, FLAGS_mail_paswd, FLAGS_mail_host, FLAGS_mail_from);
    isb.make_media_config(FLAGS_media_public_url_prefix);
    isb.make_discovery_object(FLAGS_registry_host, FLAGS_base_service);
    isb.make_rpc_object(FLAGS_listen_port, FLAGS_rpc_timeout, FLAGS_rpc_threads);
    isb.make_registry_object(FLAGS_registry_host, FLAGS_base_service + FLAGS_instance_name,
                             FLAGS_access_host);

    auto server = isb.build();
```

同时删掉旧的 `FLAGS_file_service` 引用（`make_discovery_object` 不再需要 `file_service_name` 参数）。

- [ ] **Step 2: 删掉不再使用的 gflag**

```bash
# 删除这一行（不再需要 FileService 引用）：
# DEFINE_string(file_service, "/service/file_service", "文件管理子服务名称");
```

- [ ] **Step 3: Commit**

```bash
git add user/source/user_server.cc
git commit -m "identity: main 同步 IdentityServerBuilder + 加 media_public_url_prefix flag"
```

---

## Task 13: 更新 user_server.conf

**Files:**
- Modify: `conf/user_server.conf`

- [ ] **Step 1: 加 media_public_url_prefix**

在 `-auth_config=/im/conf/auth.json` 之后加一行：

```
-media_public_url_prefix=https://cdn.chatnow.com/public
```

- [ ] **Step 2: Commit**

```bash
git add conf/user_server.conf
git commit -m "conf(identity): 加 media_public_url_prefix 配置"
```

---

## Task 14: 全仓 grep 验证旧引用清零

**Files:**
- None (只读验证)

- [ ] **Step 1: 验证 UserServiceImpl 零引用**

```bash
grep -rn "UserServiceImpl\|UserServerBuilder\|UserServer[^B]" --include="*.h" --include="*.hpp" --include="*.cc" --include="*.cpp" . | grep -v ".git/" | grep -v docs/ | grep -v third_party/
```

Expected: 零输出（UserServerBuilder 只在 docs/ 和 .git 中出现）。

- [ ] **Step 2: 验证旧命名空间零引用**

```bash
grep -rn "chatnow::UserRegister\|chatnow::UserLogin\|chatnow::MailRegister\|chatnow::MailLogin\|chatnow::GetUserInfo\|chatnow::SetUserAvatar\|chatnow::SetUserNickname\|chatnow::SetUserDescription\|chatnow::SetUserMailNumber\|chatnow::FileService_Stub" --include="*.h" --include="*.hpp" --include="*.cc" --include="*.cpp" . | grep -v ".git/" | grep -v docs/
```

Expected: 零输出（这些旧类型引用不应再出现在源码中）。

- [ ] **Step 3: 验证 identity_service.proto 无 session_id**

```bash
grep -n "session_id" proto/identity/identity_service.proto
```

Expected: 零输出。

- [ ] **Step 4: 如有残留，修复后 commit；否则跳过**

---

## Self-Review

完成上述 14 个 Task 后自查：

- [ ] **Spec coverage**：spec §1–§9 每节是否都有 Task 落地？
  - §一 RPC 矩阵 → T4-T10
  - §二 Proto 变更 → T3
  - §三 RPC 详细设计 → T4-T10
  - §四 密码安全 bcrypt → T2
  - §五 avatar_url 拼接 → T4, T12
  - §六 Builder/Server 重构 → T11, T12
  - §七 文件清单 → 全部覆盖
  - §八 RPC 鉴权模式 → Register/SendVerifyCode 手写 try/catch，GetProfile/UpdateProfile/GetMultiUserInfo/SearchUsers 用 HANDLE_RPC
  - §九 接口契约 → 类型签名已对齐

- [ ] **Placeholder scan**：
  - "TBD" / "TODO" 只允许出现在 phone_code 分支的 NOT_IMPLEMENTED 抛异常中（有意为之）
  - 所有 code block 都有完整代码

- [ ] **Type consistency 检查**：
  - `::chatnow::identity::RegisterReq/Rsp` 字段名与 T5 一致
  - `fill_user_info` 参数类型 `const User &` 与 ODB `User` 类匹配
  - `ESUser::append_data` 签名 `(uid, mail, phone, nickname, description, avatar_id, status)` 与 T5 一致
  - `_media_public_url_prefix` 在 T4 定义、T12 注入

---

## Execution Handoff

Plan complete and saved. Two execution options:
