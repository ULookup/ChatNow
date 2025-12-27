#pragma once

#include <brpc/server.h>    //
#include <butil/logging.h>  // 实现语音识别子服务
#include "etcd.hpp"         // 服务注册模块封装
#include "channel.hpp"      // 信道管理模块封装
#include "logger.hpp"       // 日志模块封装
#include "utils.hpp"        // 基础工具接口
#include "data_es.hpp"      // es数据管理客户端封装
#include "mysql_chat_session_member.hpp"   // mysql数据管理客户端封装
#include "mysql_chat_session.hpp"   // mysql数据管理客户端封装
#include "mysql_relation.hpp"   // mysql数据管理客户端封装
#include "mysql_friend_apply.hpp"   // mysql数据管理客户端封装
#include "data_redis.hpp"   // redis数据管理客户端封装
#include "base.pb.h"
#include "user.pb.h"        // protobuf框架代码
#include "message.pb.h"
#include "friend.pb.h"


namespace chatnow
{

class FriendServiceImpl : public FriendService
{
public:
    FriendServiceImpl(const std::shared_ptr<elasticlient::Client> &es_client,
                    const std::shared_ptr<odb::core::database> &mysql_client,
                    const ServiceManager::ptr &channel_manager,
                    const std::string &user_service_name,
                    const std::string &message_service_name)
                    : _es_user(std::make_shared<ESUser>(es_client)),
                    _mysql_friend_apply(std::make_shared<FriendApplyTable>(mysql_client)),
                    _mysql_chat_session(std::make_shared<ChatSessionTable>(mysql_client)),
                    _mysql_chat_session_member(std::make_shared<ChatSessionMemberTable>(mysql_client)),
                    _mysql_relation(std::make_shared<RelationTable>(mysql_client)),
                    _user_service_name(user_service_name),
                    _message_service_name(message_service_name),
                    _mm_channels(channel_manager) {}
    ~FriendServiceImpl() = default;
    /* brief: 用户注册 */
    virtual void UserRegister(::google::protobuf::RpcController* controller,
                        const ::chatnow::UserRegisterReq* request,
                        ::chatnow::UserRegisterRsp* response,
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
    /* brief: 获取好友列表 */
    virtual void GetFriendList(::google::protobuf::RpcController* controller,
                        const ::chatnow::GetFriendListReq* request,
                        ::chatnow::GetFriendListRsp* response,
                        ::google::protobuf::Closure* done)
    {
        brpc::ClosureGuard rpc_guard(done);
        auto err_response = [this, response](const std::string &rid, const std::string &err_msg) -> void {
            response->set_request_id(rid);
            response->set_success(false);
            response->set_errmsg(err_msg);
            return;
        };
        //1. 提取请求的关键要素
        std::string rid = request->request_id();
        std::string uid = request->user_id();
        //2. 从数据库查询获取用户的好友ID
        auto friend_id_list = _mysql_relation->friends(uid);
        std::unordered_set<std::string> user_id_list;
        for(auto &id : friend_id_list) {
            user_id_list.insert(id);
        }
        //3. 从用户子服务批量获取用户信息
        std::unordered_map<std::string, UserInfo> user_list;
        bool ret = GetUserInfo(rid, user_id_list, user_list);
        if(ret == false) {
            LOG_ERROR("请求ID - {} 批量获取用户信息失败", rid);
            return err_response(rid, "批量获取用户信息失败");
        }
        //4. 组织响应
        response->set_request_id(rid);
        response->set_success(true);
        for(const auto &user_it : user_list) {
            auto user_info = response->add_friend_list();
            user_info->CopyFrom(user_it.second);
        }
    }
    /* brief: 好友删除 */
    virtual void FriendRemove(::google::protobuf::RpcController* controller,
                        const ::chatnow::FriendRemoveReq* request,
                        ::chatnow::FriendRemoveRsp* response,
                        ::google::protobuf::Closure* done)
    {
        brpc::ClosureGuard rpc_guard(done);
        auto err_response = [this, response](const std::string &rid, const std::string &err_msg) -> void {
            response->set_request_id(rid);
            response->set_success(false);
            response->set_errmsg(err_msg);
            return;
        };
        //1. 提取关键要素
        std::string rid = request->request_id();
        std::string uid = request->user_id();
        std::string pid = request->peer_id();
        //2. 从用户关系表中删除好友关系信息
        bool ret = _mysql_relation->remove(uid, pid);
        if(ret == false) {
            LOG_ERROR("请求ID - {} 从数据库删除好友信息失败", rid);
            return err_response(rid, "从数据库删除好友信息失败");
        }
        //3. 从会话信息表中，删除对应的聊天会话 -- 同时删除会话成员表中的成员信息
        ret = _mysql_chat_session->remove(uid, pid);
        if(ret == false) {
            LOG_ERROR("请求ID - {} 从数据库删除好友会话信息失败", rid);
            return err_response(rid, "从数据库删除好友会话信息失败");
        }
        //4. 组织响应
        response->set_request_id(rid);
        response->set_success(true);
    }
    /* 好友添加申请 */
    virtual void FriendAdd(::google::protobuf::RpcController* controller,
                        const ::chatnow::FriendAddReq* request,
                        ::chatnow::FriendAddRsp* response,
                        ::google::protobuf::Closure* done)
    {
        brpc::ClosureGuard rpc_guard(done);
        auto err_response = [this, response](const std::string &rid, const std::string &err_msg) -> void {
            response->set_request_id(rid);
            response->set_success(false);
            response->set_errmsg(err_msg);
            return;
        };
        //1. 提取请求中的关键要素：申请人 被申请人
        std::string rid = request->request_id();
        std::string uid = request->user_id();
        std::string pid = request->respondent_id();
        //2. 判断两人是不是已经是好友
        bool ret = _mysql_relation->exists(uid, pid);
        if(ret == true) {
            LOG_ERROR("请求ID - {} 申请好友失败，两人 {} - {} 已经是好友", rid, uid, pid);
            return err_response(rid, "两者已经是好友关系");
        }
        //3. 判断当前是否已经申请过好友
        ret = _mysql_friend_apply->exists(uid, pid);
        if(ret == true) {
            LOG_ERROR("请求ID - {} 申请好友失败，已经申请过对方好友", rid);
            return err_response(rid, "已经申请过对方好友");
        }
        //4. 向好友申请表新增申请信息
        std::string eid = uuid();
        FriendApply ev(eid, uid, pid);
        ret = _mysql_friend_apply->insert(ev);
        if(ret == false) {
            LOG_ERROR("请求ID - {} 向数据库新增好友申请事件失败", rid);
            return err_response(rid, "向数据库新增好友申请事件失败");
        }
        //5. 组织响应 
        response->set_request_id(rid);
        response->set_success(true);
        response->set_notify_event_id(eid); 
    }
    /* brief: 好友申请处理 */
    virtual void FriendAddProcess(::google::protobuf::RpcController* controller,
                        const ::chatnow::FriendAddProcessReq* request,
                        ::chatnow::FriendAddProcessRsp* response,
                        ::google::protobuf::Closure* done)
    {
        brpc::ClosureGuard rpc_guard(done);
        auto err_response = [this, response](const std::string &rid, const std::string &err_msg) -> void {
            response->set_request_id(rid);
            response->set_success(false);
            response->set_errmsg(err_msg);
            return;
        };
        //1. 提取关键要素：申请人 被申请人 处理结果 事件ID
        std::string rid = request->request_id();
        std::string eid = request->notify_event_id();
        std::string uid = request->user_id();       //被申请人
        std::string pid = request->apply_user_id(); //申请人
        bool agree =  request->agree();
        //2. 判断有没有该申请事件
        bool ret = _mysql_friend_apply->exists(pid, uid);
        if(ret == false) {
            LOG_ERROR("请求ID - {} 没有对应 {} - {} 的好友申请事件", rid, pid, uid);
            return err_response(rid, "没有找到对应的好友申请事件");
        }
        //3. 如果有：可以处理 --- 删除申请事件 --- 事件已经处理完毕
        ret = _mysql_friend_apply->remove(pid, uid);
        if(ret == false) {
            LOG_ERROR("请求ID - {} 从数据库删除申请事件 {} - {} 失败", rid, pid, uid);
            return err_response(rid, "从数据库删除申请失败");
        }
        //4. 如果结果是同意：向数据库新增好友关系信息；新增单聊会话信息及会话成员
        std::string cssid;
        if(agree == true) {
            cssid = uuid();
            ChatSession cs(cssid, "", ChatSessionType::SINGLE);
            ret = _mysql_chat_session->insert(cs);
            if(ret == false) {
                LOG_ERROR("请求ID - {} 新增会话信息 {} 失败", rid, cssid);
                return err_response(rid, "新增会话信息失败");
            }
            ChatSessionMember csm1(cssid, uid);
            ChatSessionMember csm2(cssid, pid);
            std::vector<ChatSessionMember> member_lsit = {csm1, csm2};
            ret = _mysql_chat_session_member->append(member_lsit);
            if(ret == false) {
                LOG_ERROR("请求ID - {} 从数据库删除申请事件 {} - {} 失败", rid, pid, uid);
                return err_response(rid, "从数据库删除申请失败");
            }
        }
        //5. 组织响应
        response->set_request_id(rid);
        response->set_success(true);
        response->set_new_session_id(cssid);
    }
    /* brief: 用户搜索 */
    virtual void FriendSearch(::google::protobuf::RpcController* controller,
                        const ::chatnow::FriendSearchReq* request,
                        ::chatnow::FriendSearchRsp* response,
                        ::google::protobuf::Closure* done)
    {
        brpc::ClosureGuard rpc_guard(done);
        auto err_response = [this, response](const std::string &rid, const std::string &err_msg) -> void {
            response->set_request_id(rid);
            response->set_success(false);
            response->set_errmsg(err_msg);
            return;
        };
        //1. 提取关键要素（可能是用户ID，手机号，昵称的一部分）
        std::string rid = request->request_id();
        std::string uid = request->user_id();
        std::string skey = request->search_key();
        //2. 根据获取到的用户ID，从用户子服务器进行批量用户信息获取
        auto friend_id_list = _mysql_relation->friends(uid);
        //2. 从ES搜索引擎进行用户信息搜索 -- 过滤掉当前好友
        std::unordered_set<std::string> user_id_list;
        auto search_res = _es_user->search(skey, friend_id_list);
        for(auto &it : search_res) {
            user_id_list.insert(it.user_id());
        }
        //3. 根据获取到的用户ID，从用户子服务进行批量用户信息获取
        std::unordered_map<std::string, UserInfo> user_list;
        bool ret = GetUserInfo(rid, user_id_list, user_list);
        if(ret == false) {
            LOG_ERROR("请求ID - {} 批量获取用户信息失败", rid);
            return err_response(rid, "批量获取用户信息失败");
        }
        //4. 组织响应
        response->set_request_id(rid);
        response->set_success(true);
        for(const auto &user_it : user_list) {
            auto user_info = response->add_user_info();
            user_info->CopyFrom(user_it.second);
        }
    }
    /* brief: 获取待处理的好友申请列表 */
    virtual void GetPendingFriendEventList(::google::protobuf::RpcController* controller,
                        const ::chatnow::GetPendingFriendEventListReq* request,
                        ::chatnow::GetPendingFriendEventListRsp* response,
                        ::google::protobuf::Closure* done)
    {
        brpc::ClosureGuard rpc_guard(done);
        auto err_response = [this, response](const std::string &rid, const std::string &err_msg) -> void {
            response->set_request_id(rid);
            response->set_success(false);
            response->set_errmsg(err_msg);
            return;
        };
        //1. 提取关键要素：当前用户ID
        std::string rid = request->request_id();
        std::string uid = request->user_id();
        //2. 从数据库获取待处理的申请事件信息 --- 申请人用户ID列表
        auto res = _mysql_friend_apply->apply_users(uid);
        std::unordered_set<std::string> user_id_list;
        for(auto &id : res) {
            user_id_list.insert(id);
        }
        //3. 批量获取申请人用户信息
        std::unordered_map<std::string, UserInfo> user_list;
        bool ret = GetUserInfo(rid, user_id_list, user_list);
        if(ret == false) {
            LOG_ERROR("请求ID - {} 批量获取用户信息失败", rid);
            return err_response(rid, "批量获取用户信息失败");
        }
        //4. 组织响应
        response->set_request_id(rid);
        response->set_success(true);
        for(const auto &user_it : user_list) {
            auto ev = response->add_event();
            ev->mutable_sender()->CopyFrom(user_it.second);
        }
    }
    /* brief: 获取会话列表 */
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
        //1. 提取关键要素：当前请求用户ID
        std::string rid = request->request_id();
        std::string uid = request->user_id();
        //2. 从数据库查询出用户单聊会话列表
        auto single_friend_list = _mysql_chat_session->singleChatSession(uid);
        //2.1 从单聊会话列表中，取出所有好友的ID，从用户子服务获取用户信息
        std::unordered_set<std::string> user_id_lsit;
        for(const auto &single_friend : single_friend_list) {
            user_id_lsit.insert(single_friend.friend_id);
        }
        std::unordered_map<std::string, UserInfo> user_list;
        bool ret = GetUserInfo(rid, user_id_lsit, user_list);
        if(ret == false) {
            LOG_ERROR("请求ID - {} 批量获取用户信息失败", rid);
            return err_response(rid, "批量获取用户信息失败");
        }
        //2.2 设置响应会话信息，会话名称就是好友名称；会话头像就是好友头像
        
        //3. 从数据库查询用户群里列表
        auto group_chat_list = _mysql_chat_session->groupChatSession(uid);
        //4. 根据所有的会话ID，从消息存储子服务获取会话最后一条消息
        //5. 组织响应
        for(const auto &f : single_friend_list) {
            auto chat_session_info = response->add_chat_session_info_list();
            chat_session_info->set_single_chat_friend_id(f.friend_id);
            chat_session_info->set_chat_session_id(f.chat_session_id);
            chat_session_info->set_chat_session_name(user_list[f.friend_id].nickname());
            chat_session_info->set_avatar(user_list[f.friend_id].avatar());
            MessageInfo msg;
            ret = GetRecentMsg(rid, f.chat_session_id, msg);
            if(ret == false) {
                LOG_ERROR("请求ID - {} 获取会话消息失败", rid);
                return err_response(rid, "获取会话消息失败");
            }
            chat_session_info->mutable_prev_message()->CopyFrom(msg);
        }
        for(const auto &f : group_chat_list) {
            auto chat_session_info = response->add_chat_session_info_list();
            chat_session_info->set_chat_session_id(f.chat_session_id);
            chat_session_info->set_chat_session_name(f.chat_session_name);
            MessageInfo msg;
            ret = GetRecentMsg(rid, f.chat_session_id, msg);
            if(ret == false) {
                LOG_ERROR("请求ID - {} 获取会话消息失败", rid);
                return err_response(rid, "获取会话消息失败");
            }
            chat_session_info->mutable_prev_message()->CopyFrom(msg);
        }
        response->set_request_id(rid);
        response->set_success(true);
    }
    /* brief: 创建群聊会话 */
    virtual void ChatSessionCreate(::google::protobuf::RpcController* controller,
                        const ::chatnow::ChatSessionCreateReq* request,
                        ::chatnow::ChatSessionCreateRsp* response,
                        ::google::protobuf::Closure* done)
    {
        brpc::ClosureGuard rpc_guard(done);
        auto err_response = [this, response](const std::string &rid, const std::string &err_msg) -> void {
            response->set_request_id(rid);
            response->set_success(false);
            response->set_errmsg(err_msg);
            return;
        };
        //创建会话，其实针对的是用户要创建一个群聊会话
        //1. 提取关键要素 --- 会话名称 会话成员
        std::string rid = request->request_id();
        std::string uid = request->user_id();
        std::string cssname = request->chat_session_name();
        //2. 生成会话ID，向数据库添加会话信息，添加会话成员信息
        std::string cssid = uuid();
        ChatSession cs(cssid, cssname, ChatSessionType::GROUP);
        bool ret = _mysql_chat_session->insert(cs);
        if(ret == false) {
            LOG_ERROR("请求ID - {} 向数据库添加会话信息失败: cssname = {}", rid, cssname);
            return err_response(rid, "向数据库添加会话信息失败");
        }

        std::vector<ChatSessionMember> member_list;
        for(int i = 0; i < request->member_id_list_size(); ++i) {
            ChatSessionMember csm(cssid, request->member_id_list(i));
            member_list.push_back(csm);
        }

        ret = _mysql_chat_session_member->append(member_list);
        if(ret == false) {
            LOG_ERROR("请求ID - {} 向数据库添加会话成员信息失败: cssname = {}", rid, cssname);
            return err_response(rid, "向数据库添加会话成员信息失败");
        }
        //3. 组织响应 --- 组织会话信息
        response->set_request_id(rid);
        response->set_success(true);
        response->mutable_chat_session_info()->set_chat_session_id(cssid);
        response->mutable_chat_session_info()->set_chat_session_name(cssname);
    }
    /* brief: 查看群聊信息，进行群聊信息展示 */
    virtual void GetChatSessionMember(::google::protobuf::RpcController* controller,
                        const ::chatnow::GetChatSessionMemberReq* request,
                        ::chatnow::GetChatSessionMemberRsp* response,
                        ::google::protobuf::Closure* done)
    {
        brpc::ClosureGuard rpc_guard(done);
        auto err_response = [this, response](const std::string &rid, const std::string &err_msg) -> void {
            response->set_request_id(rid);
            response->set_success(false);
            response->set_errmsg(err_msg);
            return;
        };
        //1. 提取关键要素： 聊天会话ID
        std::string rid = request->request_id();
        std::string uid = request->user_id();
        std::string cssid = request->chat_session_id();
        //2. 从数据库获取会话成员ID列表
        auto member_id_lists = _mysql_chat_session_member->members(cssid);
        std::unordered_set<std::string> uid_list;
        for(const auto &id : member_id_lists) {
            uid_list.insert(id);
        }
        //3. 从用户子服务批量获取用户信息
        std::unordered_map<std::string, UserInfo> user_list;
        bool ret = GetUserInfo(rid, uid_list, user_list);
        if(ret == false) {
            LOG_ERROR("请求ID - {} 从用户子服务获取用户信息失败", rid);
            return err_response(rid, "从用户子服务获取用户信息失败");
        }
        //4. 组织响应
        response->set_request_id(rid);
        response->set_success(true);
        for(const auto &user_it : user_list) {
            auto user_info = response->add_member_info_list();
            user_info->CopyFrom(user_it.second);
        }
    }
private:
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

        bool GetRecentMsg(const std::string &rid, const std::string &cssid, MessageInfo &msg) {
            auto channel = _mm_channels->choose(_message_service_name);
            if(!channel) {
                LOG_ERROR("请求ID - {} 没有可供访问的用户子服务节点: {}", rid, _message_service_name);
                return false;
            }
            MsgStorageService_Stub stub(channel.get());
            GetRecentMsgReq req;
            GetRecentMsgRsp rsp;
            req.set_request_id(rid);
            req.set_chat_session_id(cssid);
            req.set_msg_count(1);
            brpc::Controller cntl;
            stub.GetRecentMsg(&cntl, &req, &rsp, nullptr);
            if(cntl.Failed() == true) {
                LOG_ERROR("请求ID - {} 消息存储子服务调用失败: {}", rid, cntl.ErrorText());
                return false;
            }
            if(rsp.success() == false) {
                LOG_ERROR("请求ID - {} 获取会话 {} 最近消息失败: {}", rid, cssid, rsp.errmsg());
                return false;
            }
            if(rsp.msg_list_size() > 0) {
                msg.CopyFrom(rsp.msg_list(0));
            }
            return true;
        }
private:
    ESUser::ptr _es_user;

    FriendApplyTable::ptr _mysql_friend_apply;
    ChatSessionTable::ptr _mysql_chat_session;
    ChatSessionMemberTable::ptr _mysql_chat_session_member;
    RelationTable::ptr _mysql_relation;
    /* 以下是 RPC 调用客户端相关对象 */
    std::string _user_service_name;
    std::string _message_service_name;
    ServiceManager::ptr _mm_channels;
};

class FriendServer
{
public:
    using ptr = std::shared_ptr<FriendServer>;

    FriendServer(const Discovery::ptr &service_discover,
            const Registry::ptr &reg_client,
            const std::shared_ptr<elasticlient::Client> &es_client,
            const std::shared_ptr<odb::core::database> &mysql_client,
            const std::shared_ptr<brpc::Server> &server) 
        : _service_discover(service_discover), 
        _reg_client(reg_client), 
        _mysql_client(mysql_client), 
        _rpc_server(server) {}
    ~FriendServer() = default;
    /* brief: 搭建RPC服务器，并启动服务器 */
    void start() {
        _rpc_server->RunUntilAskedToQuit();
    }
private:
    Discovery::ptr _service_discover;
    Registry::ptr _reg_client;
    std::shared_ptr<brpc::Server> _rpc_server;
    std::shared_ptr<odb::core::database> _mysql_client;
    std::shared_ptr<sw::redis::Redis> _redis_client;
};

/* 建造者模式: 将对象真正的构造过程封装，便于后期扩展和调整 */
class FriendServerBuilder
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
    /* brief: 用于构造服务发现&信道管理客户端对象 */
    void make_discovery_object(const std::string &reg_host, 
                            const std::string &base_service_name,
                            const std::string &user_service_name,
                            const std::string &message_service_name)
    {
        _user_service_name = user_service_name;
        _message_service_name = message_service_name;
        _mm_channels = std::make_shared<ServiceManager>();
        _mm_channels->declared(_user_service_name);
        _mm_channels->declared(_message_service_name);
        LOG_DEBUG("设置用户子服务为需添加管理的子服务: {}", _user_service_name);
        LOG_DEBUG("设置消息存储子服务为需添加管理的子服务: {}", _message_service_name);
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
        if(!_es_client) {
            LOG_ERROR("还未初始化ES搜索引擎模块");
            abort();
        }
        if(!_mysql_client) {
            LOG_ERROR("还未初始化MySQL数据库模块");
            abort();
        }
        if(!_mm_channels) {
            LOG_ERROR("还未初始化信道管理模块");
            abort();
        }

        FriendServiceImpl *friend_service = new FriendServiceImpl(_es_client, _mysql_client, _mm_channels, _user_service_name, _message_service_name);
        int ret = _rpc_server->AddService(friend_service, brpc::ServiceOwnership::SERVER_OWNS_SERVICE);
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
    FriendServer::ptr build() {
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

        FriendServer::ptr server = std::make_shared<FriendServer>(_service_discover, _reg_client, _es_client, _mysql_client, _rpc_server);
        return server;
    }
private:
    Registry::ptr _reg_client;

    std::shared_ptr<elasticlient::Client> _es_client;
    std::shared_ptr<odb::core::database> _mysql_client;
    std::shared_ptr<sw::redis::Redis> _redis_client;

    std::string _user_service_name;
    std::string _message_service_name;
    ServiceManager::ptr _mm_channels;
    Discovery::ptr _service_discover;

    std::shared_ptr<brpc::Server> _rpc_server;
};

}