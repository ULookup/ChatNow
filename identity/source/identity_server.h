#include <brpc/server.h>    //
#include <butil/logging.h>  // 实现语音识别子服务
#include "infra/etcd.hpp"         // 服务注册模块封装
#include "infra/logger.hpp"       // 日志模块封装
#include "utils/utils.hpp"        // 基础工具接口
#include "infra/mail.hpp"         // 邮箱验证
#include "dao/data_es.hpp"      // es数据管理客户端封装
#include "dao/mysql_user.hpp"   // mysql数据管理客户端封装
#include "dao/data_redis.hpp"   // redis数据管理客户端封装
#include "common/types.pb.h"
#include "common/error.pb.h"
#include "common/envelope.pb.h"
#include "identity/identity_service.pb.h"

#include "auth/auth_context.hpp"
#include "auth/jwt_codec.hpp"
#include "auth/jwt_store.hpp"
#include "auth/bcrypt_util.hpp"
#include "auth/auth_config_loader.hpp"
#include "error/error_codes.hpp"
#include "error/service_error.hpp"


namespace chatnow
{

/**
 * IdentityServiceImpl —— 完整 Identity 服务实现（9/9 RPC）
 * Login/Logout/RefreshToken：JWT 签发/吊销/滚动
 * Register/SendVerifyCode/GetProfile/UpdateProfile/GetMultiUserInfo/SearchUsers
 */
class IdentityServiceImpl : public ::chatnow::identity::IdentityService
{
public:
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

    /* brief: IdentityService.Login —— 用户名密码登录，签发 access+refresh */
    void Login(::google::protobuf::RpcController* controller,
               const ::chatnow::identity::LoginReq* request,
               ::chatnow::identity::LoginRsp* response,
               ::google::protobuf::Closure* done) override
    {
        brpc::ClosureGuard rpc_guard(done);
        auto* header = response->mutable_header();
        header->set_request_id(request->request_id());
        try {
            // 1. 凭据校验
            if (request->has_phone_code()) {
                throw ServiceError(::chatnow::error::kNotImplemented,
                                   "phone_code login not yet supported");
            }
            if (!request->has_username_pwd()) {
                throw ServiceError(::chatnow::error::kAuthInvalidCredentials,
                                   "no valid credential provided");
            }
            const auto& cred = request->username_pwd();
            auto user = _mysql_user->select_by_nickname(cred.username());
            if (!user || !auth::check_password(cred.password(), user->password())) {
                throw ServiceError(::chatnow::error::kAuthInvalidCredentials,
                                   "username or password incorrect");
            }
            // 2. device_id 取自请求（P2 不强制校验，P3 收紧）
            std::string device_id = request->device_id();
            if (device_id.empty()) device_id = "unknown_device";

            // 3. 签发 access + refresh
            std::string access_jti  = auth::JwtCodec::random_jti();
            std::string refresh_jti = auth::JwtCodec::random_jti();
            std::string access_tok  = _jwt_codec->sign_access(user->user_id(), device_id, access_jti);
            std::string refresh_tok = _jwt_codec->sign_refresh(user->user_id(), device_id, refresh_jti);

            // 4. 写入 refresh 反查表（用于后续 Logout / Rotate）
            _jwt_store->put_active_refresh(user->user_id(), device_id,
                                           refresh_jti, _jwt_codec->refresh_ttl_sec());

            // 5. 返回 AuthTokens + UserInfo
            auto* tokens = response->mutable_tokens();
            tokens->set_access_token(access_tok);
            tokens->set_refresh_token(refresh_tok);
            tokens->set_access_expires_in_sec(_jwt_codec->access_ttl_sec());
            tokens->set_refresh_expires_in_sec(_jwt_codec->refresh_ttl_sec());

            auto* uinfo = response->mutable_user_info();
            uinfo->set_user_id(user->user_id());
            uinfo->set_nickname(user->nickname());

            header->set_success(true);
            header->set_error_code(::chatnow::common::OK);
            LOG_INFO("Login OK rid={} uid={} did={}", request->request_id(),
                     user->user_id(), device_id);
        } catch (const ServiceError& e) {
            header->set_success(false);
            header->set_error_code(e.code());
            header->set_error_message(e.message());
            LOG_WARN("Login 失败 rid={} code={} msg={}",
                     request->request_id(), e.code(), e.message());
        } catch (const std::exception& e) {
            header->set_success(false);
            header->set_error_code(::chatnow::error::kSystemInternalError);
            header->set_error_message("internal error");
            LOG_ERROR("Login 异常 rid={}: {}", request->request_id(), e.what());
        }
    }

    /* brief: IdentityService.Logout —— 吊销当前设备的 access + refresh
     * metadata 必填：x-user-id / x-device-id / x-jwt-jti（P1 extract_auth）
     */
    void Logout(::google::protobuf::RpcController* controller,
                const ::chatnow::identity::LogoutReq* request,
                ::chatnow::identity::LogoutRsp* response,
                ::google::protobuf::Closure* done) override
    {
        brpc::ClosureGuard rpc_guard(done);
        auto* cntl = static_cast<brpc::Controller*>(controller);
        auto* header = response->mutable_header();
        header->set_request_id(request->request_id());
        try {
            auto ctx = ::chatnow::auth::extract_auth(cntl);
            // 1. 吊销当前 access token（按剩余寿命 TTL，避免黑名单膨胀）
            //    精确剩余寿命需要解 token；用 access_ttl_sec 上限保守覆盖
            if (!ctx.jwt_jti.empty()) {
                _jwt_store->revoke(ctx.jwt_jti, _jwt_codec->access_ttl_sec());
            }
            // 2. 吊销该设备的 refresh
            std::string rt_jti = _jwt_store->get_active_refresh(ctx.user_id, ctx.device_id);
            if (!rt_jti.empty()) {
                _jwt_store->revoke(rt_jti, _jwt_codec->refresh_ttl_sec());
                _jwt_store->clear_active_refresh(ctx.user_id, ctx.device_id);
            }
            header->set_success(true);
            header->set_error_code(::chatnow::common::OK);
            LOG_INFO("Logout OK rid={} uid={} did={}", request->request_id(),
                     ctx.user_id, ctx.device_id);
        } catch (const ServiceError& e) {
            header->set_success(false);
            header->set_error_code(e.code());
            header->set_error_message(e.message());
            LOG_WARN("Logout 失败 rid={} code={}", request->request_id(), e.code());
        } catch (const std::exception& e) {
            header->set_success(false);
            header->set_error_code(::chatnow::error::kSystemInternalError);
            header->set_error_message("internal error");
            LOG_ERROR("Logout 异常 rid={}: {}", request->request_id(), e.what());
        }
    }

    /* brief: IdentityService.RefreshToken —— 滚动刷新 + 重放检测（spec §2.3） */
    void RefreshToken(::google::protobuf::RpcController* controller,
                      const ::chatnow::identity::RefreshTokenReq* request,
                      ::chatnow::identity::RefreshTokenRsp* response,
                      ::google::protobuf::Closure* done) override
    {
        brpc::ClosureGuard rpc_guard(done);
        auto* header = response->mutable_header();
        header->set_request_id(request->request_id());
        try {
            // 1. 验签 refresh token（强制 typ=refresh）
            auto claims = _jwt_codec->verify(request->refresh_token(),
                                             /*require_refresh=*/true);
            // 2. 黑名单检查
            if (_jwt_store->is_revoked(claims.jti)) {
                throw ServiceError(::chatnow::error::kAuthTokenInvalid,
                                   "refresh token revoked");
            }
            // 3. 反查表是否还指向这个 refresh_jti
            std::string active = _jwt_store->get_active_refresh(claims.sub, claims.did);
            if (active != claims.jti) {
                _jwt_store->revoke(claims.jti, _jwt_codec->refresh_ttl_sec());
                _jwt_store->clear_active_refresh(claims.sub, claims.did);
                throw ServiceError(::chatnow::error::kAuthRefreshTokenReused,
                                   "refresh token mismatch active");
            }
            // 4. 颁发新 access + 新 refresh
            std::string new_access_jti  = auth::JwtCodec::random_jti();
            std::string new_refresh_jti = auth::JwtCodec::random_jti();
            std::string new_access  = _jwt_codec->sign_access(claims.sub, claims.did, new_access_jti);
            std::string new_refresh = _jwt_codec->sign_refresh(claims.sub, claims.did, new_refresh_jti);
            // 5. 原子滚动 + 重放链检测
            auto rot = _jwt_store->rotate_refresh_or_detect_reuse(
                claims.sub, claims.did, claims.jti, new_refresh_jti,
                _jwt_codec->refresh_ttl_sec());
            if (rot == auth::JwtStore::RotateResult::kReuseDetected) {
                _jwt_store->revoke(claims.jti, _jwt_codec->refresh_ttl_sec());
                _jwt_store->clear_active_refresh(claims.sub, claims.did);
                throw ServiceError(::chatnow::error::kAuthRefreshTokenReused,
                                   "refresh chain reuse detected");
            }
            // 6. 旧 refresh 写黑名单（剩余寿命）
            int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            int remain = static_cast<int>(claims.exp_sec - now);
            if (remain > 0) {
                _jwt_store->revoke(claims.jti, remain);
            }

            auto* tokens = response->mutable_tokens();
            tokens->set_access_token(new_access);
            tokens->set_refresh_token(new_refresh);
            tokens->set_access_expires_in_sec(_jwt_codec->access_ttl_sec());
            tokens->set_refresh_expires_in_sec(_jwt_codec->refresh_ttl_sec());

            header->set_success(true);
            header->set_error_code(::chatnow::common::OK);
            LOG_INFO("RefreshToken OK rid={} uid={} did={}", request->request_id(),
                     claims.sub, claims.did);
        } catch (const ServiceError& e) {
            header->set_success(false);
            header->set_error_code(e.code());
            header->set_error_message(e.message());
            LOG_WARN("RefreshToken 失败 rid={} code={}",
                     request->request_id(), e.code());
        } catch (const std::exception& e) {
            header->set_success(false);
            header->set_error_code(::chatnow::error::kSystemInternalError);
            header->set_error_message("internal error");
            LOG_ERROR("RefreshToken 异常 rid={}: {}", request->request_id(), e.what());
        }
    }

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
                if (!nickname_check(nickname)) {
                    throw ServiceError(::chatnow::error::kAuthInvalidCredentials,
                                       "nickname invalid");
                }
                if (!password_check(cred.password())) {
                    throw ServiceError(::chatnow::error::kAuthInvalidCredentials,
                                       "password invalid");
                }
                auto existing = _mysql_user->select_by_nickname(nickname);
                if (existing) {
                    throw ServiceError(::chatnow::error::kAuthUserAlreadyExists,
                                       "nickname already taken");
                }
                user_id = uuid();
                std::string hash = auth::hash_password(cred.password());
                auto user = std::make_shared<User>(user_id, nickname, hash);
                if (!_mysql_user->insert(user)) {
                    throw ServiceError(::chatnow::error::kSystemInternalError,
                                       "db insert failed");
                }
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
                std::string code = verifyCode();
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

    // ---- 工具方法 ----
    std::string uuid() { return utils::uuid(); }

    std::string make_avatar_url(const std::string &avatar_id) {
        if (avatar_id.empty() || _media_public_url_prefix.empty()) return "";
        return _media_public_url_prefix + "/" + avatar_id;
    }

    // ---- UserInfo 组装（后续 RPC 共用） ----
    void fill_user_info(::chatnow::common::UserInfo *u, const User &user) {
        u->set_user_id(user.user_id());
        u->set_nickname(user.nickname());
        if (!user.description().empty()) u->set_bio(user.description());
        if (!user.phone().empty()) u->set_phone(user.phone());
        u->set_avatar_url(make_avatar_url(user.avatar_id()));
    }
};

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
    /* brief: 搭建RPC服务器，并启动服务器 */
    void start() {
        _rpc_server->RunUntilAskedToQuit();
    }
private:
    Discovery::ptr _service_discover;
    Registry::ptr _reg_client;
    std::shared_ptr<brpc::Server> _rpc_server;
    std::shared_ptr<elasticlient::Client> _es_client;
    std::shared_ptr<odb::core::database> _mysql_client;
    std::shared_ptr<sw::redis::Redis> _redis_client;
};

/* 建造者模式: 将对象真正的构造过程封装，便于后期扩展和调整 */
class IdentityServerBuilder
{
public:
    /* brief: 构造es客户端对象 */
    void make_es_object(const std::vector<std::string> host_list) { _es_client = ESClientFactory::create(host_list); }
    /* brief: 构造mysql客户端对象 */
    void make_mysql_object(const std::string &user,
                        const std::string &password,
                        const std::string &host,
                        const std::string &db,
                        const std::string &cset,
                        uint16_t port,
                        int conn_pool_count)
    {
        _mysql_client = ODBFactory::create(user, password, host, db, cset, port, conn_pool_count);
    }
    /* brief: 构造redis客户端对象 */
    void make_redis_object(const std::string &host,
                        uint16_t port,
                        int db,
                        bool keep_alive)
    {
        _redis_client = RedisClientFactory::create(host, port, db, keep_alive);
    }
    /* brief: 加载 JWT 配置并构造 codec / store（必须在 make_redis_object 之后） */
    void make_jwt_object(const std::string &auth_config_path) {
        if (!_redis_client) {
            LOG_ERROR("make_jwt_object 必须在 make_redis_object 之后调用");
            abort();
        }
        auto cfg = ::chatnow::auth::load_jwt_config_from_file(auth_config_path);
        _jwt_codec = std::make_shared<::chatnow::auth::JwtCodec>(std::move(cfg));
        _jwt_store = std::make_shared<::chatnow::auth::JwtStore>(_redis_client);
    }
    /* brief: 构造邮箱验证客户端 */
    void make_mail_object(const std::string &mail_username,
                        const std::string &mail_password,
                        const std::string &mail_url,
                        const std::string &mail_from)
    {
        mail_settings settings = {
            .username = mail_username,
            .password = mail_password,
            .url = mail_url,
            .from = mail_from
        };
        _mail_client = std::make_shared<MailClient>(settings);
    }
    /* brief: 注入媒体资源公开访问 URL 前缀（用于 avatar_url 拼接） */
    void make_media_config(const std::string &public_url_prefix) {
        _media_public_url_prefix = public_url_prefix;
        while (!_media_public_url_prefix.empty() && _media_public_url_prefix.back() == '/')
            _media_public_url_prefix.pop_back();
    }
    /* brief: 用于构造服务发现客户端对象（Identity 不声明服务依赖，仅维持 etcd 拓扑监听） */
    void make_discovery_object(const std::string &reg_host,
                            const std::string &base_service_name)
    {
        auto put_cb = [](const std::string &name, const std::string &host) {
            LOG_INFO("Discovery put: {} @ {}", name, host);
        };
        auto del_cb = [](const std::string &name, const std::string &host) {
            LOG_INFO("Discovery del: {} @ {}", name, host);
        };
        _service_discover = std::make_shared<Discovery>(reg_host, base_service_name, put_cb, del_cb);
    }
    /* brief: 用于构造服务注册客户端对象 */
    void make_registry_object(const std::string &reg_host,
                        const std::string &service_name,
                        const std::string &access_host) {
        _reg_client = std::make_shared<Registry>(reg_host);
        _reg_client->registry(service_name, access_host);
    }
    /* brief: 构造RPC对象，只注册 IdentityServiceImpl */
    void make_rpc_object(uint16_t port, uint32_t timeout, uint8_t num_threads) {
        _rpc_server = std::make_shared<brpc::Server>();
        if(!_es_client) {
            LOG_ERROR("还未初始化ES搜索引擎模块");
            abort();
        }
        if(!_mysql_client) {
            LOG_ERROR("还未初始化MySQL数据库模块");
            abort();
        }
        if(!_redis_client) {
            LOG_ERROR("还未初始化Redis数据库模块");
            abort();
        }
        if(!_mail_client) {
            LOG_ERROR("还未初始化邮件验证客户端模块");
            abort();
        }
        if(!_jwt_codec || !_jwt_store) {
            LOG_ERROR("还未初始化JWT模块（缺 make_jwt_object）");
            abort();
        }

        IdentityServiceImpl *identity_service = new IdentityServiceImpl(
            _mysql_client, _es_client, _redis_client, _mail_client,
            _jwt_codec, _jwt_store, _media_public_url_prefix);
        int ret = _rpc_server->AddService(identity_service, brpc::ServiceOwnership::SERVER_OWNS_SERVICE);
        if(ret == -1) {
            LOG_ERROR("添加IdentityService RPC服务失败!");
            abort();
        }

        brpc::ServerOptions options;
        options.idle_timeout_sec = timeout;
        options.num_threads = num_threads;
        ret = _rpc_server->Start(port, &options);
        if(ret == -1) {
            LOG_ERROR("服务启动失败!");
            abort();
        }
    }
    IdentityServer::ptr build() {
        if(!_service_discover) {
            LOG_ERROR("还未初始化服务发现模块");
            abort();
        }
        if(!_reg_client) {
            LOG_ERROR("还未初始化服务注册模块");
            abort();
        }
        if(!_rpc_server) {
            LOG_ERROR("还未初始化RPC服务器模块");
            abort();
        }

        IdentityServer::ptr server = std::make_shared<IdentityServer>(
            _service_discover, _reg_client, _es_client, _mysql_client, _redis_client, _rpc_server);
        return server;
    }
private:
    Registry::ptr _reg_client;

    std::shared_ptr<elasticlient::Client> _es_client;
    std::shared_ptr<odb::core::database> _mysql_client;
    std::shared_ptr<sw::redis::Redis> _redis_client;
    std::shared_ptr<MailClient> _mail_client;

    std::string _media_public_url_prefix;
    Discovery::ptr _service_discover;

    std::shared_ptr<::chatnow::auth::JwtCodec> _jwt_codec;
    std::shared_ptr<::chatnow::auth::JwtStore> _jwt_store;

    std::shared_ptr<brpc::Server> _rpc_server;
};

}