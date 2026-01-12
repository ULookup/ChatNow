#pragma once

#include <brpc/server.h>    //
#include <butil/logging.h>  // 实现语音识别子服务
#include "etcd.hpp"         // 服务注册模块封装
#include "channel.hpp"      // 信道管理模块封装
#include "logger.hpp"       // 日志模块封装
#include "utils.hpp"        // 基础工具接口
#include "mysql_chat_session_member.hpp"   // mysql数据管理客户端封装
#include "mysql_chat_session.hpp"   // mysql数据管理客户端封装
#include "base.pb.h"
#include "chatsession.pb.h"

namespace chatnow
{

class ChatSessionServiceImpl : public ChatSessionService
{
public:
    ChatSessionServiceImpl(const std::shared_ptr<odb::core::database> &mysql_client,
                        const ServiceManager::ptr &channel_manager)
                        : _mysql_chat_session(std::make_shared<ChatSessionTable>(mysql_client)),
                        _mysql_chat_session_member(std::make_shared<ChatSessionMemberTable>(mysql_client)),
                        _mm_channels(channel_manager) {}
    ~ChatSessionServiceImpl() = default;
    virtual void GetChatSessionList(::google::protobuf::RpcController* controller,
                       const ::chatnow::GetChatSessionListReq* request,
                       ::chatnow::GetChatSessionListRsp* response,
                       ::google::protobuf::Closure* done)
    {
        brpc::ClosureGuard rpc_guard(done);
        auto err_response = [this, response](const std::string &rid, const std::string &err_msg) -> void {
            response->set_request_id(rid);
            response->set_success(false);
            response->set_errmsg(err_msg);
            return;
        };
    }
    virtual void GetChatSessionDetail(::google::protobuf::RpcController* controller,
                        const ::chatnow::GetChatSessionDetailReq* request,
                        ::chatnow::GetChatSessionDetailRsp* response,
                        ::google::protobuf::Closure* done);
    virtual void ChatSessionCreate(::google::protobuf::RpcController* controller,
                        const ::chatnow::ChatSessionCreateReq* request,
                        ::chatnow::ChatSessionCreateRsp* response,
                        ::google::protobuf::Closure* done);
    virtual void GetChatSessionMember(::google::protobuf::RpcController* controller,
                        const ::chatnow::GetChatSessionMemberReq* request,
                        ::chatnow::GetChatSessionMemberRsp* response,
                        ::google::protobuf::Closure* done);
    virtual void SetChatSessionName(::google::protobuf::RpcController* controller,
                        const ::chatnow::SetChatSessionNameReq* request,
                        ::chatnow::SetChatSessionNameRsp* response,
                        ::google::protobuf::Closure* done);
    virtual void SetChatSessionAvatar(::google::protobuf::RpcController* controller,
                        const ::chatnow::SetChatSessionAvatarReq* request,
                        ::chatnow::SetChatSessionAvatarRsp* response,
                        ::google::protobuf::Closure* done);
    virtual void AddChatSessionMember(::google::protobuf::RpcController* controller,
                        const ::chatnow::AddChatSessionMemberReq* request,
                        ::chatnow::AddChatSessionMemberRsp* response,
                        ::google::protobuf::Closure* done);
    virtual void RemoveChatSessionMember(::google::protobuf::RpcController* controller,
                        const ::chatnow::RemoveChatSessionMemberReq* request,
                        ::chatnow::RemoveChatSessionMemberRsp* response,
                        ::google::protobuf::Closure* done);
    virtual void TransferChatSessionOwner(::google::protobuf::RpcController* controller,
                        const ::chatnow::TransferChatSessionOwnerReq* request,
                        ::chatnow::TransferChatSessionOwnerRsp* response,
                        ::google::protobuf::Closure* done);
    virtual void ModifyMemberPermission(::google::protobuf::RpcController* controller,
                        const ::chatnow::ModifyMemberPermissionReq* request,
                        ::chatnow::ModifyMemberPermissionRsp* response,
                        ::google::protobuf::Closure* done);
    virtual void ModifyChatSessionStatus(::google::protobuf::RpcController* controller,
                        const ::chatnow::ModifyChatSessionStatusReq* request,
                        ::chatnow::ModifyChatSessionStatusRsp* response,
                        ::google::protobuf::Closure* done);
    virtual void SearchChatSession(::google::protobuf::RpcController* controller,
                        const ::chatnow::SearchChatSessionReq* request,
                        ::chatnow::SearchChatSessionRsp* response,
                        ::google::protobuf::Closure* done);
    virtual void GetMemberRoleList(::google::protobuf::RpcController* controller,
                        const ::chatnow::GetMemberRoleListReq* request,
                        ::chatnow::GetMemberRoleListRsp* response,
                        ::google::protobuf::Closure* done);
    virtual void SetSessionMuted(::google::protobuf::RpcController* controller,
                        const ::chatnow::SetSessionMutedReq* request,
                        ::chatnow::SetSessionMutedRsp* response,
                        ::google::protobuf::Closure* done);
    virtual void SetSessionPinned(::google::protobuf::RpcController* controller,
                        const ::chatnow::SetSessionPinnedReq* request,
                        ::chatnow::SetSessionPinnedRsp* response,
                        ::google::protobuf::Closure* done);
    virtual void SetSessionVisible(::google::protobuf::RpcController* controller,
                        const ::chatnow::SetSessionVisibleReq* request,
                        ::chatnow::SetSessionVisibleRsp* response,
                        ::google::protobuf::Closure* done);
    virtual void GetUserSessionStatus(::google::protobuf::RpcController* controller,
                        const ::chatnow::GetUserSessionStatusReq* request,
                        ::chatnow::GetUserSessionStatusRsp* response,
                        ::google::protobuf::Closure* done);
    virtual void QuitChatSession(::google::protobuf::RpcController* controller,
                        const ::chatnow::QuitChatSessionReq* request,
                        ::chatnow::QuitChatSessionRsp* response,
                        ::google::protobuf::Closure* done);
private:
       
private:
    ChatSessionTable::ptr _mysql_chat_session;
    ChatSessionMemberTable::ptr _mysql_chat_session_member;
    /* 以下是 RPC 调用客户端相关对象 */
    ServiceManager::ptr _mm_channels;
};

class ChatSessionServer
{
public:
    using ptr = std::shared_ptr<ChatSessionServer>;

    ChatSessionServer(const Discovery::ptr &service_discover,
            const Registry::ptr &reg_client,
            const std::shared_ptr<odb::core::database> &mysql_client,
            const std::shared_ptr<brpc::Server> &server) 
        : _service_discover(service_discover), 
        _reg_client(reg_client), 
        _mysql_client(mysql_client), 
        _rpc_server(server) {}
    ~ChatSessionServer() = default;
    /* brief: 搭建RPC服务器，并启动服务器 */
    void start() {
        _rpc_server->RunUntilAskedToQuit();
    }
private:
    Discovery::ptr _service_discover;
    Registry::ptr _reg_client;
    std::shared_ptr<brpc::Server> _rpc_server;
    std::shared_ptr<odb::core::database> _mysql_client;
};

/* 建造者模式: 将对象真正的构造过程封装，便于后期扩展和调整 */
class ChatSessionServerBuilder
{
public:
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
    /* brief: 用于构造服务发现&信道管理客户端对象 */
    void make_discovery_object(const std::string &reg_host, 
                            const std::string &base_service_name)
    {
        _mm_channels = std::make_shared<ServiceManager>();
        auto put_cb = std::bind(&ServiceManager::onServiceOnline, _mm_channels.get(), std::placeholders::_1, std::placeholders::_2);
        auto del_cb = std::bind(&ServiceManager::onServiceOffline, _mm_channels.get(), std::placeholders::_1, std::placeholders::_2);

        _service_discover = std::make_shared<Discovery>(reg_host, base_service_name, put_cb, del_cb);
    }
    /* brief: 用于构造服务注册客户端对象 */
    void make_registry_object(const std::string &reg_host,
                        const std::string &service_name,
                        const std::string &access_host) {
        _reg_client = std::make_shared<Registry>(reg_host);
        _reg_client->registry(service_name, access_host);
    }
    /* brief: 构造RPC对象 */
    void make_rpc_object(uint16_t port, uint32_t timeout, uint8_t num_threads) {
        _rpc_server = std::make_shared<brpc::Server>();
        if(!_mysql_client) {
            LOG_ERROR("还未初始化MySQL数据库模块");
            abort();
        }
        if(!_mm_channels) {
            LOG_ERROR("还未初始化信道管理模块");
            abort();
        }

        ChatSessionServiceImpl *chatsession_service = new ChatSessionServiceImpl(_mysql_client, _mm_channels);
        int ret = _rpc_server->AddService(chatsession_service, brpc::ServiceOwnership::SERVER_OWNS_SERVICE);
        if(ret == -1) {
            LOG_ERROR("添加RPC服务失败!");
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
    ChatSessionServer::ptr build() {
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

        ChatSessionServer::ptr server = std::make_shared<ChatSessionServer>(_service_discover, _reg_client, _mysql_client, _rpc_server);
        return server;
    }
private:
    Registry::ptr _reg_client;

    std::shared_ptr<odb::core::database> _mysql_client;

    ServiceManager::ptr _mm_channels;
    Discovery::ptr _service_discover;

    std::shared_ptr<brpc::Server> _rpc_server;
};

} // namespace chatnow;