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
#include "user.pb.h"
#include "file.pb.h"
#include "message.pb.h"

namespace chatnow
{

class ChatSessionServiceImpl : public ChatSessionService
{
public:
    ChatSessionServiceImpl(const std::shared_ptr<odb::core::database> &mysql_client,
                        const ServiceManager::ptr &channel_manager,
                        const std::string &user_service_name,
                        const std::string &file_service_name,
                        const std::string &message_service_name)
                        : _mysql_chat_session(std::make_shared<ChatSessionTable>(mysql_client)),
                        _mysql_chat_session_member(std::make_shared<ChatSessionMemberTable>(mysql_client)),
                        _mm_channels(channel_manager),
                        _user_service_name(user_service_name),
                        _file_service_name(file_service_name),
                        _message_service_name(message_service_name) {}
    ~ChatSessionServiceImpl() = default;
    /* brief: 获取聊天会话列表 */
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
        //1.  提取请求关键信息
        std::string rid = request->request_id();
        std::string uid = request->user_id();
        //2.  从数据库查询出用户的单聊会话列表
        auto single_chat_session_list = _mysql_chat_session->singleChatSession(uid);
        //2.1 从单聊会话列表中，取出所有好友的ID，从用户子服务获取用户信息
        std::unordered_set<std::string> user_id_list;
        for(const auto &single_chat_session : single_chat_session_list) {
            user_id_list.insert(single_chat_session.friend_id);
        }
        std::unordered_map<std::string, UserInfo> user_list;
        bool ret = GetUserInfo(rid, user_id_list, user_list);
        if(ret == false) {
            LOG_ERROR("请求ID - {} 批量获取用户信息失败", rid);
            return err_response(rid, "批量获取用户信息失败");
        }
        //3. 从数据库查询出用户的群聊会话列表
        auto group_chat_session_list = _mysql_chat_session->groupChatSession(uid);
        //4. 组织响应
        for(const auto &single_chat_session : single_chat_session_list) {
            auto chat_session_info = response->add_chat_session_info_list();
            chat_session_info->set_single_chat_friend_id(single_chat_session.friend_id);
            chat_session_info->set_chat_session_id(single_chat_session.chat_session_id);
            chat_session_info->set_chat_session_name(user_list[single_chat_session.friend_id].nickname());
            chat_session_info->set_avatar(user_list[single_chat_session.friend_id].avatar());

            MessageInfo msg;
            ret = GetRecentMsg(rid, single_chat_session.chat_session_id, msg);
            if(ret == false) { continue; }
            chat_session_info->mutable_prev_message()->CopyFrom(msg);
            //===================== v2.0 ========================

        }
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
    /* brief: 对用户管理子服务调用的封装 */
    bool GetUserInfo(const std::string &rid,
                    const std::unordered_set<std::string> &uid_list,
                    std::unordered_map<std::string, UserInfo> &user_list)
    {
        auto channel = _mm_channels->choose(_user_service_name);
        if(!channel) {
            LOG_ERROR("请求ID - {} 没有可供访问的用户子服务节点: {}", rid, _user_service_name);
            return false;
        }
        UserService_Stub stub(channel.get());
        GetMultiUserInfoReq req;
        GetMultiUserInfoRsp rsp;
        req.set_request_id(rid);
        for(auto &id : uid_list) {
            req.add_users_id(id);
        }
        brpc::Controller cntl;
        stub.GetMultiUserInfo(&cntl, &req, &rsp, nullptr);
        if(cntl.Failed() == true) {
            LOG_ERROR("请求ID - {} 用户子服务调用失败: {}", rid, cntl.ErrorText());
            return false;
        }
        if(rsp.success() == false) {
            LOG_ERROR("请求ID - {} 批量获取用户信息失败: {}", rid, rsp.errmsg());
            return false;
        }
        for(const auto &user_it : rsp.users_info()) {
            user_list.insert(std::make_pair(user_it.first, user_it.second));
        }

        return true;
    }
    /* brief: 对消息存储子服务管理的封装 */
    bool GetRecentMsg(const std::string &rid,
                    const std::string &chat_session_id,
                    MessageInfo &msg)
    {
        auto channel = _mm_channels->choose(_message_service_name);
        if(!channel) {
            LOG_ERROR("请求ID - {} 没有可供访问的消息子服务节点: {}", rid, _user_service_name);
            return false;
        }
        MsgStorageService_Stub stub(channel.get());
        GetRecentMsgReq req;
        GetRecentMsgRsp rsp;
        req.set_request_id(rid);
        req.set_chat_session_id(chat_session_id);
        req.set_msg_count(1);
        brpc::Controller cntl;
        stub.GetRecentMsg(&cntl, &req, &rsp, nullptr);
        if(cntl.Failed() == true) {
            LOG_ERROR("请求ID - {} 消息子服务调用失败: {}", rid, cntl.ErrorText());
            return false;
        }
        if(rsp.success() == false) {
            LOG_ERROR("请求ID - {} 获取消息信息失败: {}", rid, rsp.errmsg());
            return false;
        }
        if(rsp.msg_list_size() > 0) {
            msg.CopyFrom(rsp.msg_list(0));
            return true;
        }
        LOG_DEBUG("请求ID - {} 没有获取到消息", rid);
        return false;
    }
private:
    ChatSessionTable::ptr _mysql_chat_session;
    ChatSessionMemberTable::ptr _mysql_chat_session_member;
    /* 以下是 RPC 调用客户端相关对象 */
    ServiceManager::ptr _mm_channels;
    std::string _user_service_name;
    std::string _file_service_name;
    std::string _message_service_name;
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
                            const std::string &base_service_name,
                            const std::string &user_service_name,
                            const std::string &file_service_name,
                            const std::string &message_service_name)
    {
        _user_service_name = user_service_name;
        _file_service_name = file_service_name;
        _message_service_name = message_service_name;
        _mm_channels = std::make_shared<ServiceManager>();
        _mm_channels->declared(user_service_name);
        _mm_channels->declared(file_service_name);
        LOG_DEBUG("设置用户子服务为需添加管理的子服务: {}", _user_service_name);
        LOG_DEBUG("设置消息存储子服务为需添加管理的子服务: {}", _file_service_name);
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

        ChatSessionServiceImpl *chatsession_service = new ChatSessionServiceImpl(_mysql_client, _mm_channels, _user_service_name, _file_service_name, _message_service_name);
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
    std::string _user_service_name;
    std::string _file_service_name;
    std::string _message_service_name;
};

} // namespace chatnow;