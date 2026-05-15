#pragma once

#include <brpc/server.h>
#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include "infra/etcd.hpp"
#include "mq/channel.hpp"
#include "infra/logger.hpp"
#include "utils/utils.hpp"
#include "dao/data_es.hpp"
#include "dao/mysql_chat_session_member.hpp"
#include "dao/mysql_chat_session.hpp"
#include "dao/mysql_relation.hpp"
#include "dao/mysql_friend_apply.hpp"
#include "dao/mysql_user_block.hpp"
#include "common/types.pb.h"
#include "common/error.pb.h"
#include "common/envelope.pb.h"
#include "identity/identity_service.pb.h"
#include "conversation/conversation_service.pb.h"
#include "relationship/relationship_service.pb.h"

#include "auth/auth_context.hpp"
#include "auth/forward_auth.hpp"
#include "error/error_codes.hpp"
#include "error/handle_rpc.hpp"
#include "error/service_error.hpp"
#include "log/log_context.hpp"

namespace chatnow {

class RelationshipServiceImpl : public ::chatnow::relationship::RelationshipService
{
public:
    RelationshipServiceImpl(const std::shared_ptr<elasticlient::Client> &es_client,
                            const std::shared_ptr<odb::core::database> &mysql_client,
                            const ServiceManager::ptr &channel_manager,
                            const std::string &identity_service_name,
                            const std::string &conversation_service_name)
        : _es_user(std::make_shared<ESUser>(es_client)),
          _mysql_friend_apply(std::make_shared<FriendApplyTable>(mysql_client)),
          _mysql_relation(std::make_shared<RelationTable>(mysql_client)),
          _mysql_user_block(std::make_shared<UserBlockTable>(mysql_client)),
          _identity_service_name(identity_service_name),
          _conversation_service_name(conversation_service_name),
          _mm_channels(channel_manager) {}

    ~RelationshipServiceImpl() override = default;

    // ---- 4 个读取类 RPC ----

    void ListFriends(::google::protobuf::RpcController* base_cntl,
                     const ::chatnow::relationship::ListFriendsReq* req,
                     ::chatnow::relationship::ListFriendsRsp* rsp,
                     ::google::protobuf::Closure* done) override
    {
        brpc::ClosureGuard done_guard(done);
        auto* cntl = static_cast<brpc::Controller*>(base_cntl);
        HANDLE_RPC(cntl, req, rsp, {
            auto friend_ids = _mysql_relation->friends(auth.user_id);
            std::unordered_set<std::string> uid_set(friend_ids.begin(), friend_ids.end());

            std::unordered_map<std::string, ::chatnow::common::UserInfo> user_map;
            if (!uid_set.empty()) {
                if (!fetch_users(cntl, req->request_id(), uid_set, user_map))
                    throw ServiceError(::chatnow::error::kSystemUnavailable,
                                       "identity service unavailable");
            }
            for (auto &kv : user_map) {
                auto* u = rsp->add_friend_list();
                u->CopyFrom(kv.second);
            }
            // page 字段：当前 DAO 还没分页参数，先全量返回；page.has_more=false
            rsp->mutable_page()->set_has_more(false);
            rsp->mutable_page()->set_total_count(static_cast<int32_t>(friend_ids.size()));
        });
    }

    void SearchFriends(::google::protobuf::RpcController* base_cntl,
                       const ::chatnow::relationship::SearchFriendsReq* req,
                       ::chatnow::relationship::SearchFriendsRsp* rsp,
                       ::google::protobuf::Closure* done) override
    {
        brpc::ClosureGuard done_guard(done);
        auto* cntl = static_cast<brpc::Controller*>(base_cntl);
        HANDLE_RPC(cntl, req, rsp, {
            // 1. 排除集合：自己 + 已是好友 + (我拉黑 ∪ 拉黑我)
            std::vector<std::string> exclude;
            exclude.push_back(auth.user_id);
            auto friends_v = _mysql_relation->friends(auth.user_id);
            exclude.insert(exclude.end(), friends_v.begin(), friends_v.end());
            auto block_set = _mysql_user_block->blocked_or_blocking(auth.user_id);
            exclude.insert(exclude.end(), block_set.begin(), block_set.end());

            // 2. ES 搜索
            auto hits = _es_user->search(req->search_key(), exclude);
            std::unordered_set<std::string> uid_set;
            for (auto &h : hits) uid_set.insert(h.user_id());

            // 3. 批量取 UserInfo
            std::unordered_map<std::string, ::chatnow::common::UserInfo> user_map;
            if (!uid_set.empty()) {
                if (!fetch_users(cntl, req->request_id(), uid_set, user_map))
                    throw ServiceError(::chatnow::error::kSystemUnavailable,
                                       "identity service unavailable");
            }
            for (auto &kv : user_map) {
                auto* u = rsp->add_user_info();
                u->CopyFrom(kv.second);
            }
        });
    }

    void ListPendingRequests(::google::protobuf::RpcController* base_cntl,
                             const ::chatnow::relationship::ListPendingReq* req,
                             ::chatnow::relationship::ListPendingRsp* rsp,
                             ::google::protobuf::Closure* done) override
    {
        brpc::ClosureGuard done_guard(done);
        auto* cntl = static_cast<brpc::Controller*>(base_cntl);
        HANDLE_RPC(cntl, req, rsp, {
            auto pendings = _mysql_friend_apply->select_pending(auth.user_id);
            std::unordered_set<std::string> uid_set;
            for (auto &row : pendings) uid_set.insert(row.user_id());

            std::unordered_map<std::string, ::chatnow::common::UserInfo> user_map;
            if (!uid_set.empty()) {
                if (!fetch_users(cntl, req->request_id(), uid_set, user_map))
                    throw ServiceError(::chatnow::error::kSystemUnavailable,
                                       "identity service unavailable");
            }
            for (auto &row : pendings) {
                auto it = user_map.find(row.user_id());
                if (it == user_map.end()) continue; // 申请人已注销/查不到，跳过
                auto* ev = rsp->add_event();
                ev->set_event_id(row.event_id());
                ev->mutable_sender()->CopyFrom(it->second);
            }
        });
    }

    void ListBlockedUsers(::google::protobuf::RpcController* base_cntl,
                          const ::chatnow::relationship::ListBlockedReq* req,
                          ::chatnow::relationship::ListBlockedRsp* rsp,
                          ::google::protobuf::Closure* done) override
    {
        brpc::ClosureGuard done_guard(done);
        auto* cntl = static_cast<brpc::Controller*>(base_cntl);
        HANDLE_RPC(cntl, req, rsp, {
            // PageRequest.cursor 当前不解析（DAO 用 offset/limit 简单分页）
            int limit  = req->page().limit() > 0 ? req->page().limit() : 50;
            int offset = 0;
            if (!req->page().cursor().empty()) {
                try { offset = std::stoi(req->page().cursor()); }
                catch (...) { offset = 0; }
            }
            auto blocked_ids = _mysql_user_block->list_blocked(auth.user_id, offset, limit);
            std::unordered_set<std::string> uid_set(blocked_ids.begin(), blocked_ids.end());

            std::unordered_map<std::string, ::chatnow::common::UserInfo> user_map;
            if (!uid_set.empty()) {
                if (!fetch_users(cntl, req->request_id(), uid_set, user_map))
                    throw ServiceError(::chatnow::error::kSystemUnavailable,
                                       "identity service unavailable");
            }
            for (auto &id : blocked_ids) {
                auto it = user_map.find(id);
                if (it == user_map.end()) continue;
                auto* u = rsp->add_blocked_list();
                u->CopyFrom(it->second);
            }
            int64_t total = _mysql_user_block->count_blocked(auth.user_id);
            rsp->mutable_page()->set_total_count(static_cast<int32_t>(total));
            rsp->mutable_page()->set_has_more(offset + limit < total);
            if (rsp->page().has_more()) {
                rsp->mutable_page()->set_next_cursor(std::to_string(offset + limit));
            }
        });
    }

    // ---- 4 个写入类 RPC ----

    void SendFriendRequest(::google::protobuf::RpcController* base_cntl,
                           const ::chatnow::relationship::SendFriendReq* req,
                           ::chatnow::relationship::SendFriendRsp* rsp,
                           ::google::protobuf::Closure* done) override
    {
        brpc::ClosureGuard done_guard(done);
        auto* cntl = static_cast<brpc::Controller*>(base_cntl);
        HANDLE_RPC(cntl, req, rsp, {
            const std::string &uid = auth.user_id;
            const std::string &pid = req->respondent_id();
            if (pid.empty() || uid == pid)
                throw ServiceError(::chatnow::error::kSystemInvalidArgument,
                                   "invalid respondent_id");

            // 对端拉黑了我 → 直接 BLOCKED
            if (_mysql_user_block->is_blocked(pid, uid))
                throw ServiceError(::chatnow::error::kRelationshipBlocked,
                                   "blocked by peer");

            if (_mysql_relation->exists(uid, pid))
                throw ServiceError(::chatnow::error::kRelationshipAlreadyFriends,
                                   "already friends");

            // PENDING 中 → 直接复用之前 event_id（与现状一致）
            if (_mysql_friend_apply->exists_pending(uid, pid)) {
                auto latest = _mysql_friend_apply->select_latest(uid, pid);
                if (latest && latest->status() == FriendApplyStatus::PENDING) {
                    rsp->set_notify_event_id(latest->event_id());
                    return; // HANDLE_RPC 已写好成功 header
                }
                throw ServiceError(::chatnow::error::kRelationshipRequestPending,
                                   "duplicate apply");
            }

            // 上一次被拒 + 距今 <72h → 拒绝
            auto last = _mysql_friend_apply->select_latest(uid, pid);
            if (last && last->status() == FriendApplyStatus::REJECTED) {
                auto now = boost::posix_time::microsec_clock::universal_time();
                if (now - last->create_time() < boost::posix_time::hours(72))
                    throw ServiceError(::chatnow::error::kRelationshipRequestPending,
                                       "rejected within 72h");
            }

            // 新建申请
            std::string eid = uuid();
            FriendApply ev(eid, uid, pid, FriendApplyStatus::PENDING,
                           boost::posix_time::microsec_clock::universal_time());
            if (!_mysql_friend_apply->insert(ev))
                throw ServiceError(::chatnow::error::kSystemInternalError,
                                   "insert friend_apply failed");
            rsp->set_notify_event_id(eid);
        });
    }

    void RemoveFriend(::google::protobuf::RpcController* base_cntl,
                      const ::chatnow::relationship::RemoveFriendReq* req,
                      ::chatnow::relationship::RemoveFriendRsp* rsp,
                      ::google::protobuf::Closure* done) override
    {
        brpc::ClosureGuard done_guard(done);
        auto* cntl = static_cast<brpc::Controller*>(base_cntl);
        HANDLE_RPC(cntl, req, rsp, {
            const std::string &uid = auth.user_id;
            const std::string &pid = req->peer_id();
            if (pid.empty() || uid == pid)
                throw ServiceError(::chatnow::error::kSystemInvalidArgument,
                                   "invalid peer_id");
            if (!_mysql_relation->exists(uid, pid))
                throw ServiceError(::chatnow::error::kRelationshipNotFriends,
                                   "not friends");
            if (!_mysql_relation->remove(uid, pid))
                throw ServiceError(::chatnow::error::kSystemInternalError,
                                   "remove relation failed");
            // user_block 表与好友关系独立，删好友不动 user_block；
            // 现状会话清理改由 Conversation 服务负责（不在本服务范围内）。
        });
    }

    void BlockUser(::google::protobuf::RpcController* base_cntl,
                   const ::chatnow::relationship::BlockUserReq* req,
                   ::chatnow::relationship::BlockUserRsp* rsp,
                   ::google::protobuf::Closure* done) override
    {
        brpc::ClosureGuard done_guard(done);
        auto* cntl = static_cast<brpc::Controller*>(base_cntl);
        HANDLE_RPC(cntl, req, rsp, {
            const std::string &uid = auth.user_id;
            const std::string &pid = req->peer_id();
            if (pid.empty() || uid == pid)
                throw ServiceError(::chatnow::error::kSystemInvalidArgument,
                                   "invalid peer_id");
            if (!_mysql_user_block->insert(uid, pid))
                throw ServiceError(::chatnow::error::kSystemInternalError,
                                   "insert user_block failed");
        });
    }

    void UnblockUser(::google::protobuf::RpcController* base_cntl,
                     const ::chatnow::relationship::UnblockUserReq* req,
                     ::chatnow::relationship::UnblockUserRsp* rsp,
                     ::google::protobuf::Closure* done) override
    {
        brpc::ClosureGuard done_guard(done);
        auto* cntl = static_cast<brpc::Controller*>(base_cntl);
        HANDLE_RPC(cntl, req, rsp, {
            const std::string &uid = auth.user_id;
            const std::string &pid = req->peer_id();
            if (pid.empty() || uid == pid)
                throw ServiceError(::chatnow::error::kSystemInvalidArgument,
                                   "invalid peer_id");
            if (!_mysql_user_block->remove(uid, pid))
                throw ServiceError(::chatnow::error::kSystemInternalError,
                                   "remove user_block failed");
        });
    }

private:
    bool fetch_users(brpc::Controller* in_cntl,
                     const std::string &rid,
                     const std::unordered_set<std::string> &uid_set,
                     std::unordered_map<std::string, ::chatnow::common::UserInfo> &out)
    {
        auto channel = _mm_channels->choose(_identity_service_name);
        if (!channel) {
            LOG_ERROR("rid={} identity 子服务节点不可达 svc={}",
                      rid, _identity_service_name);
            return false;
        }
        ::chatnow::identity::IdentityService_Stub stub(channel.get());
        ::chatnow::identity::GetMultiUserInfoReq  q;
        ::chatnow::identity::GetMultiUserInfoRsp  a;
        q.set_request_id(rid);
        for (auto &id : uid_set) q.add_users_id(id);

        brpc::Controller out_cntl;
        ::chatnow::auth::forward_auth_metadata(in_cntl, &out_cntl);
        stub.GetMultiUserInfo(&out_cntl, &q, &a, nullptr);
        if (out_cntl.Failed()) {
            LOG_ERROR("rid={} GetMultiUserInfo brpc 失败: {}", rid, out_cntl.ErrorText());
            return false;
        }
        if (!a.header().success()) {
            LOG_ERROR("rid={} GetMultiUserInfo 业务失败: code={} msg={}",
                      rid, a.header().error_code(), a.header().error_message());
            return false;
        }
        for (auto &kv : a.users_info()) out.emplace(kv.first, kv.second);
        return true;
    }

private:
    ESUser::ptr                  _es_user;
    FriendApplyTable::ptr        _mysql_friend_apply;
    RelationTable::ptr           _mysql_relation;
    UserBlockTable::ptr          _mysql_user_block;

    std::string _identity_service_name;
    std::string _conversation_service_name;
    ServiceManager::ptr _mm_channels;
};

class RelationshipServer
{
public:
    using ptr = std::shared_ptr<RelationshipServer>;

    RelationshipServer(const Discovery::ptr &service_discover,
                       const Registry::ptr &reg_client,
                       const std::shared_ptr<odb::core::database> &mysql_client,
                       const std::shared_ptr<brpc::Server> &server)
        : _service_discover(service_discover),
          _reg_client(reg_client),
          _mysql_client(mysql_client),
          _rpc_server(server) {}

    ~RelationshipServer() = default;
    void start() { _rpc_server->RunUntilAskedToQuit(); }

private:
    Discovery::ptr _service_discover;
    Registry::ptr  _reg_client;
    std::shared_ptr<odb::core::database> _mysql_client;
    std::shared_ptr<brpc::Server>        _rpc_server;
};

class RelationshipServerBuilder
{
public:
    void make_es_object(const std::vector<std::string> host_list) {
        _es_client = ESClientFactory::create(host_list);
    }
    void make_mysql_object(const std::string &user, const std::string &password,
                           const std::string &host, const std::string &db,
                           const std::string &cset, uint16_t port, int pool)
    {
        _mysql_client = ODBFactory::create(user, password, host, db, cset, port, pool);
    }
    void make_discovery_object(const std::string &reg_host,
                               const std::string &base_service_name,
                               const std::string &identity_service_name,
                               const std::string &conversation_service_name)
    {
        _identity_service_name     = identity_service_name;
        _conversation_service_name = conversation_service_name;
        _mm_channels = std::make_shared<ServiceManager>();
        _mm_channels->declared(_identity_service_name);
        _mm_channels->declared(_conversation_service_name);

        auto put_cb = std::bind(&ServiceManager::onServiceOnline, _mm_channels.get(),
                                std::placeholders::_1, std::placeholders::_2);
        auto del_cb = std::bind(&ServiceManager::onServiceOffline, _mm_channels.get(),
                                std::placeholders::_1, std::placeholders::_2);

        _service_discover = std::make_shared<Discovery>(reg_host, base_service_name, put_cb, del_cb);
    }
    void make_registry_object(const std::string &reg_host,
                              const std::string &service_name,
                              const std::string &access_host)
    {
        _reg_client = std::make_shared<Registry>(reg_host);
        _reg_client->registry(service_name, access_host);
    }
    void make_rpc_object(uint16_t port, uint32_t timeout, uint8_t num_threads) {
        _rpc_server = std::make_shared<brpc::Server>();
        if(!_es_client)    { LOG_ERROR("还未初始化ES模块");      abort(); }
        if(!_mysql_client) { LOG_ERROR("还未初始化MySQL模块");   abort(); }
        if(!_mm_channels)  { LOG_ERROR("还未初始化信道管理模块"); abort(); }

        auto *impl = new RelationshipServiceImpl(_es_client, _mysql_client, _mm_channels,
                                                  _identity_service_name,
                                                  _conversation_service_name);
        if(_rpc_server->AddService(impl, brpc::ServiceOwnership::SERVER_OWNS_SERVICE) == -1) {
            LOG_ERROR("添加RPC服务失败!"); abort();
        }
        brpc::ServerOptions options;
        options.idle_timeout_sec = timeout;
        options.num_threads      = num_threads;
        if(_rpc_server->Start(port, &options) == -1) {
            LOG_ERROR("服务启动失败!"); abort();
        }
    }
    RelationshipServer::ptr build() {
        if(!_service_discover) { LOG_ERROR("还未初始化服务发现模块"); abort(); }
        if(!_reg_client)       { LOG_ERROR("还未初始化服务注册模块"); abort(); }
        if(!_rpc_server)       { LOG_ERROR("还未初始化RPC模块");      abort(); }
        return std::make_shared<RelationshipServer>(_service_discover, _reg_client,
                                                    _mysql_client, _rpc_server);
    }

private:
    Registry::ptr  _reg_client;
    std::shared_ptr<elasticlient::Client> _es_client;
    std::shared_ptr<odb::core::database>  _mysql_client;

    std::string _identity_service_name;
    std::string _conversation_service_name;
    ServiceManager::ptr _mm_channels;
    Discovery::ptr      _service_discover;

    std::shared_ptr<brpc::Server> _rpc_server;
};

} // namespace chatnow
