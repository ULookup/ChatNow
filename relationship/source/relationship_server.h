#pragma once

#include <brpc/server.h>
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

    // 9 个 RPC handler 在 Task 5/6/7 实现，先编译通过用 default override
    // 注：brpc 自动给 ::SERVICE_NAME stub 提供 default 实现，未实现的 RPC
    // 走 NOT_IMPLEMENTED；但本仓约定服务端要显式 override，因此 Task 5+ 会逐个补全。

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
