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
#include "dao/data_redis.hpp"
#include "dao/mysql.hpp"
#include "dao/mysql_conversation.hpp"
#include "dao/mysql_conversation_member.hpp"
#include "common/types.pb.h"
#include "common/error.pb.h"
#include "common/envelope.pb.h"
#include "identity/identity_service.pb.h"
#include "message/message_service.pb.h"
#include "message/message_types.pb.h"
#include "conversation/conversation_service.pb.h"

#include "auth/auth_context.hpp"
#include "auth/forward_auth.hpp"
#include "error/error_codes.hpp"
#include "error/handle_rpc.hpp"
#include "error/service_error.hpp"
#include "log/log_context.hpp"

namespace chatnow {

struct ConversationServiceConfig {
    std::string public_url_prefix;
};

class ConversationServiceImpl : public ::chatnow::conversation::ConversationService
{
public:
    ConversationServiceImpl(const std::shared_ptr<elasticlient::Client> &es_client,
                            const std::shared_ptr<odb::core::database> &mysql_client,
                            const Members::ptr &members_cache,
                            const ServiceManager::ptr &channel_manager,
                            const std::string &identity_service_name,
                            const std::string &media_service_name,
                            const std::string &message_service_name,
                            const ConversationServiceConfig &cfg)
        : _es_conv(std::make_shared<ESConversation>(es_client)),
          _mysql_conv(std::make_shared<ConversationTable>(mysql_client)),
          _mysql_member(std::make_shared<ConversationMemberTable>(mysql_client)),
          _members_cache(members_cache),
          _identity_service_name(identity_service_name),
          _media_service_name(media_service_name),
          _message_service_name(message_service_name),
          _mm_channels(channel_manager),
          _cfg(cfg) {}

    ~ConversationServiceImpl() override = default;

    // —— 18 个 RPC 占位实现（T10–T14 替换） ——
    #define _PLACEHOLDER_RPC(Method, Req, Rsp) \
        void Method(::google::protobuf::RpcController* base_cntl, \
                    const ::chatnow::conversation::Req* req, \
                    ::chatnow::conversation::Rsp* rsp, \
                    ::google::protobuf::Closure* done) override { \
            brpc::ClosureGuard done_guard(done); \
            (void)base_cntl; \
            rsp->mutable_header()->set_request_id(req->request_id()); \
            rsp->mutable_header()->set_success(false); \
            rsp->mutable_header()->set_error_code( \
                ::chatnow::error::kSystemInternalError); \
            rsp->mutable_header()->set_error_message("not implemented"); \
        }
    _PLACEHOLDER_RPC(ListConversations,  ListConversationsReq,  ListConversationsRsp)
    _PLACEHOLDER_RPC(GetConversation,    GetConversationReq,    GetConversationRsp)
    _PLACEHOLDER_RPC(CreateConversation, CreateConversationReq, CreateConversationRsp)
    _PLACEHOLDER_RPC(UpdateConversation, UpdateConversationReq, UpdateConversationRsp)
    _PLACEHOLDER_RPC(DismissConversation,DismissConversationReq,DismissConversationRsp)
    _PLACEHOLDER_RPC(AddMembers,         AddMembersReq,         AddMembersRsp)
    _PLACEHOLDER_RPC(RemoveMembers,      RemoveMembersReq,      RemoveMembersRsp)
    _PLACEHOLDER_RPC(TransferOwner,      TransferOwnerReq,      TransferOwnerRsp)
    _PLACEHOLDER_RPC(ChangeMemberRole,   ChangeMemberRoleReq,   ChangeMemberRoleRsp)
    _PLACEHOLDER_RPC(ListMembers,        ListMembersReq,        ListMembersRsp)
    _PLACEHOLDER_RPC(SetMute,            SetMuteReq,            SetMuteRsp)
    _PLACEHOLDER_RPC(SetPin,             SetPinReq,             SetPinRsp)
    _PLACEHOLDER_RPC(SetVisible,         SetVisibleReq,         SetVisibleRsp)
    _PLACEHOLDER_RPC(QuitConversation,   QuitConversationReq,   QuitConversationRsp)
    _PLACEHOLDER_RPC(MarkRead,           MarkReadReq,           MarkReadRsp)
    _PLACEHOLDER_RPC(SaveDraft,          SaveDraftReq,          SaveDraftRsp)
    _PLACEHOLDER_RPC(SearchConversations,SearchConversationsReq,SearchConversationsRsp)
    _PLACEHOLDER_RPC(GetMemberIds,       GetMemberIdsReq,       GetMemberIdsRsp)
    #undef _PLACEHOLDER_RPC

private:
    ESConversation::ptr           _es_conv;
    ConversationTable::ptr        _mysql_conv;
    ConversationMemberTable::ptr  _mysql_member;
    Members::ptr                  _members_cache;
    std::string                   _identity_service_name;
    std::string                   _media_service_name;
    std::string                   _message_service_name;
    ServiceManager::ptr           _mm_channels;
    ConversationServiceConfig     _cfg;
};

class ConversationServer
{
public:
    using ptr = std::shared_ptr<ConversationServer>;
    ConversationServer(const Discovery::ptr &service_discover,
                       const Registry::ptr &reg_client,
                       const std::shared_ptr<odb::core::database> &mysql_client,
                       const std::shared_ptr<brpc::Server> &server)
        : _service_discover(service_discover),
          _reg_client(reg_client),
          _mysql_client(mysql_client),
          _rpc_server(server) {}

    ~ConversationServer() = default;
    void start() { _rpc_server->RunUntilAskedToQuit(); }

private:
    Discovery::ptr _service_discover;
    Registry::ptr  _reg_client;
    std::shared_ptr<odb::core::database> _mysql_client;
    std::shared_ptr<brpc::Server>        _rpc_server;
};

class ConversationServerBuilder
{
public:
    void make_es_object(const std::vector<std::string> host_list) {
        _es_client = ESClientFactory::create(host_list);
    }
    void make_redis_object(const std::string &host, uint16_t port, int db,
                           bool keep_alive, int pool_size) {
        _redis_client  = RedisClientFactory::create(host, port, db, keep_alive, pool_size);
        _members_cache = std::make_shared<Members>(_redis_client);
    }
    void make_mysql_object(const std::string &user, const std::string &password,
                           const std::string &host, const std::string &dbname,
                           const std::string &cset, uint16_t port, int pool)
    {
        _mysql_client = ODBFactory::create(user, password, host, dbname, cset, port, pool);
    }
    void make_discovery_object(const std::string &reg_host,
                               const std::string &base_service_name,
                               const std::string &identity_service_name,
                               const std::string &media_service_name,
                               const std::string &message_service_name)
    {
        _identity_service_name = identity_service_name;
        _media_service_name    = media_service_name;
        _message_service_name  = message_service_name;
        _mm_channels = std::make_shared<ServiceManager>();
        _mm_channels->declared(_identity_service_name);
        _mm_channels->declared(_media_service_name);
        _mm_channels->declared(_message_service_name);

        auto put_cb = std::bind(&ServiceManager::onServiceOnline, _mm_channels.get(),
                                std::placeholders::_1, std::placeholders::_2);
        auto del_cb = std::bind(&ServiceManager::onServiceOffline, _mm_channels.get(),
                                std::placeholders::_1, std::placeholders::_2);
        _service_discover = std::make_shared<Discovery>(reg_host, base_service_name, put_cb, del_cb);
    }
    void make_config(const std::string &public_url_prefix) {
        _cfg.public_url_prefix = public_url_prefix;
    }
    void make_rpc_object(uint16_t port, uint32_t timeout, uint8_t num_threads) {
        _rpc_server = std::make_shared<brpc::Server>();
        if(!_es_client)    { LOG_ERROR("还未初始化ES模块");      abort(); }
        if(!_mysql_client) { LOG_ERROR("还未初始化MySQL模块");   abort(); }
        if(!_mm_channels)  { LOG_ERROR("还未初始化信道管理模块"); abort(); }
        if(!_members_cache){ LOG_ERROR("还未初始化Members缓存");  abort(); }

        auto *impl = new ConversationServiceImpl(
            _es_client, _mysql_client, _members_cache, _mm_channels,
            _identity_service_name, _media_service_name, _message_service_name, _cfg);
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
    void make_registry_object(const std::string &reg_host,
                              const std::string &service_name,
                              const std::string &access_host)
    {
        _reg_client = std::make_shared<Registry>(reg_host);
        _reg_client->registry(service_name, access_host);
    }
    ConversationServer::ptr build() {
        if(!_service_discover) { LOG_ERROR("还未初始化服务发现模块"); abort(); }
        if(!_reg_client)       { LOG_ERROR("还未初始化服务注册模块"); abort(); }
        if(!_rpc_server)       { LOG_ERROR("还未初始化RPC模块");      abort(); }
        return std::make_shared<ConversationServer>(_service_discover, _reg_client,
                                                    _mysql_client, _rpc_server);
    }

private:
    std::shared_ptr<elasticlient::Client>   _es_client;
    std::shared_ptr<sw::redis::Redis>       _redis_client;
    Members::ptr                            _members_cache;
    std::shared_ptr<odb::core::database>    _mysql_client;
    Discovery::ptr                          _service_discover;
    Registry::ptr                           _reg_client;
    ServiceManager::ptr                     _mm_channels;
    std::string                             _identity_service_name;
    std::string                             _media_service_name;
    std::string                             _message_service_name;
    ConversationServiceConfig               _cfg;
    std::shared_ptr<brpc::Server>           _rpc_server;
};

} // namespace chatnow
