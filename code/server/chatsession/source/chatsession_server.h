#pragma once

#include <brpc/server.h>    //
#include <butil/logging.h>  // 实现语音识别子服务
#include "etcd.hpp"         // 服务注册模块封装
#include "channel.hpp"      // 信道管理模块封装
#include "logger.hpp"       // 日志模块封装
#include "utils.hpp"        // 基础工具接口
#include "mysql_chat_session_member.hpp"   // mysql数据管理客户端封装
#include "mysql_chat_session.hpp"   // mysql数据管理客户端封装
#include "data_es.hpp"
#include "base.pb.h"
#include "chatsession.pb.h"
#include "user.pb.h"
#include "file.pb.h"
#include "message.pb.h"

namespace chatnow
{

#define NORMAL_SESSION    0
#define ARCHIVED_SESSION  1
#define DISMISSED_SESSION 2

class ChatSessionServiceImpl : public ChatSessionService
{
public:
    ChatSessionServiceImpl(const std::shared_ptr<elasticlient::Client> &es_client,
                        const std::shared_ptr<odb::core::database> &mysql_client,
                        const ServiceManager::ptr &channel_manager,
                        const std::string &user_service_name,
                        const std::string &file_service_name,
                        const std::string &message_service_name)
                        : _es_chat_session(std::make_shared<ESChatSession>(es_client)),
                        _mysql_chat_session(std::make_shared<ChatSessionTable>(mysql_client)),
                        _mysql_chat_session_member(std::make_shared<ChatSessionMemberTable>(mysql_client)),
                        _mm_channels(channel_manager),
                        _user_service_name(user_service_name),
                        _file_service_name(file_service_name),
                        _message_service_name(message_service_name) 
    {
        _es_chat_session->create_index();
    }
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
        //2.  从数据库按顺序查询出用户的会话列表
        std::vector<OrderedChatSessionView> chat_session_list = _mysql_chat_session_member->list_ordered_by_user(uid);
        //3.  找到单聊会话对应的好友ID
        //3. 从数据库查询出用户单聊会话列表
        auto single_friend_list = _mysql_chat_session->singleChatSession(uid);
        std::unordered_map<std::string, std::string> single_friend_map; //会话ID 和 单聊对象ID的映射
        for (const auto &row : single_friend_list) {
            single_friend_map[row.chat_session_id] = row.friend_id;
        }
        //4.  组织响应
        for(const auto &chat_session : chat_session_list) {
            auto chat_session_info = response->add_chat_session_info_list();
            if(chat_session.session_type == ChatSessionType::SINGLE) {
                auto it = single_friend_map.find(chat_session.session_id);
                if(it != single_friend_map.end()) {
                    chat_session_info->set_single_chat_friend_id(single_friend_map[chat_session.session_id]);
                } else {
                    //数据异常，只要是单聊，就一定能找到对方 id
                    LOG_ERROR("单聊会话ID: {} 没有找到对方ID的映射", chat_session.session_id);
                }
            }
            chat_session_info->set_chat_session_id(chat_session.session_id);
            chat_session_info->set_chat_session_name(chat_session.session_name);
            if (!chat_session.avatar_id.null()) {
                chat_session_info->set_avatar(chat_session.avatar_id.get());
            }
            chat_session_info->set_chat_session_type(static_cast<uint32_t>(chat_session.session_type));
            chat_session_info->set_create_time(boost::posix_time::to_time_t(chat_session.create_time));
            chat_session_info->set_member_count(chat_session.member_count);
            chat_session_info->set_status(static_cast<ChatSessionStatus>(chat_session.status));
            auto member_info = chat_session_info->mutable_member_info_brief();
            member_info->set_is_muted(chat_session.muted);
            member_info->set_is_visible(chat_session.visible);
            // 置顶时间（有值才设置）
            if (!chat_session.pin_time.null()) {
                member_info->set_pin_time(
                    boost::posix_time::to_time_t(chat_session.pin_time.get())
                );
            }
            //================================================================//
            //最近一条消息
            MessageInfo msg;
            bool ret = GetRecentMsg(rid, chat_session.session_id, msg);
            if(ret == false) { continue; }
            chat_session_info->mutable_prev_message()->CopyFrom(msg);
            //================================================================//
        }
        response->set_request_id(rid);
        response->set_success(true);
    }
    /* brief: 获取单个会话的详细信息 */
    virtual void GetChatSessionDetail(::google::protobuf::RpcController* controller,
                        const ::chatnow::GetChatSessionDetailReq* request,
                        ::chatnow::GetChatSessionDetailRsp* response,
                        ::google::protobuf::Closure* done)
    {
        brpc::ClosureGuard rpc_guard(done);
        auto err_response = [this, response](const std::string &rid, const std::string &err_msg) -> void {
            response->set_request_id(rid);
            response->set_success(false);
            response->set_errmsg(err_msg);
            return;
        };
        //1.  取出请求关键要素
        std::string rid  = request->request_id();
        std::string uid  = request->user_id();
        std::string ssid = request->chat_session_id();
        //2.  从数据库取出该会话的详细信息
        std::shared_ptr<ChatSession> chatSession  = _mysql_chat_session->select(ssid);
        if (!chatSession) {
            LOG_ERROR("请求ID - {} 从数据库取出该会话 {} 的详细信息失败", rid, ssid);
            return err_response(rid, "从数据库取出该会话的详细信息失败");
        }   
        std::shared_ptr<ChatSessionMember> member = _mysql_chat_session_member->select(ssid, uid);
        if (!member) {
            LOG_ERROR("请求ID - {} 用户没在会话 {} 中", rid, ssid);
            return err_response(rid, "用户没在会话中");
        }
        //3.  组织响应
        auto chat_session_info = response->mutable_chat_session_info();
        chat_session_info->set_chat_session_id(chatSession->chat_session_id());
        chat_session_info->set_chat_session_name(chatSession->chat_session_name());
        chat_session_info->set_avatar(chatSession->avatar_id());
        chat_session_info->set_chat_session_type(static_cast<uint32_t>(chatSession->chat_session_type()));
        chat_session_info->set_create_time(boost::posix_time::to_time_t(chatSession->create_time()));
        chat_session_info->set_member_count(chatSession->member_count());
        chat_session_info->set_status(static_cast<ChatSessionStatus>(chatSession->status()));
        auto member_info_detail = chat_session_info->mutable_member_info_detail();
        member_info_detail->set_chat_session_id(ssid);
        member_info_detail->set_user_id(uid);
        if(member->last_read_msg() != 0) {
            member_info_detail->set_last_message_id(member->last_read_msg());
        }
        member_info_detail->set_is_muted(member->muted());
        member_info_detail->set_is_visible(member->visible());
        if(member->pin_time().is_not_a_date_time()) {
            member_info_detail->set_pin_time(boost::posix_time::to_time_t(member->pin_time()));
        }
        member_info_detail->set_role(static_cast<ChatSessionMemberRole>(member->role()));
        member_info_detail->set_join_time(boost::posix_time::to_time_t(member->join_time()));
        
        response->set_request_id(rid);
        response->set_success(true);
    }
    /* brief: 创建聊天会话 */
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
        //1.  提取请求关键要素
        std::string rid     = request->request_id();
        std::string uid     = request->user_id();
        std::string cssName = request->chat_session_name();
        std::vector<std::string> members;
        for(auto &e : request->member_id_list()) {
            members.push_back(e);
        }
        //2.  创建会话
        std::string cssID = uuid();
        ChatSession chatSession(cssID, 
                                cssName, 
                                ChatSessionType::GROUP, 
                                boost::posix_time::second_clock::local_time(),
                                request->member_id_list_size(),
                                NORMAL_SESSION);
        //3.  向数据库插入创建的会话
        bool ret = _mysql_chat_session->insert(chatSession);
        if(ret == false) {
            LOG_ERROR("请求ID - {} 向数据库添加会话信息失败: cssName = {}", rid, cssName);
            return err_response(rid, "向数据库添加会话信息失败");
        }
        ret = _es_chat_session->append_data(chatSession);
        if(ret == false) {
            LOG_ERROR("请求ID - {} 向ES添加会话信息失败: cssName = {}", rid, cssName);
            return err_response(rid, "向ES添加会话信息失败");
        }
        //4.  向会话成员表插入数据
        std::vector<ChatSessionMember> member_list;
        for(int i = 0; i < request->member_id_list_size(); ++i) {
            if(request->member_id_list(i) == uid) {
                ChatSessionMember chatSessionMember(cssID, request->member_id_list(i), false, true, ChatSessionRole::OWNER, boost::posix_time::second_clock::local_time());
                member_list.push_back(chatSessionMember);
            } else {
                ChatSessionMember chatSessionMember(cssID, request->member_id_list(i), false, true, ChatSessionRole::NORMAL, boost::posix_time::second_clock::local_time());
                member_list.push_back(chatSessionMember);
            }
        }
        ret = _mysql_chat_session_member->append(member_list);
        if(ret == false) {
            LOG_ERROR("请求ID - {} 向数据库添加会话成员信息失败", rid);
            return err_response(rid, "向数据库添加会话成员信息失败");
        }
        //5.  组织响应
        response->set_request_id(rid);
        response->set_success(true);
    }
    /* brief: 获取会话成员列表 */
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
        //1.  提取关键要素
        std::string rid   = request->request_id();
        std::string uid   = request->user_id();
        std::string cssID = request->chat_session_id();
        LOG_DEBUG("要查询的会话ID {}", cssID);
        //2.  从数据库查询出该会话所有成员信息
        std::vector<ChatSessionMemberRoleView> chatSessionMember_list = _mysql_chat_session_member->list_member_roles(cssID);
        std::unordered_set<std::string> user_id_list;
        for(auto &member : chatSessionMember_list) {
            //2.1 取出用户ID
            user_id_list.insert(member.user_id);
        }
        //2.2 调用用户管理子服务，取出用户详细信息
        std::unordered_map<std::string, UserInfo> user_list;
        bool ret = GetUserInfo(rid, user_id_list, user_list);
        if(ret == false) {
            LOG_ERROR("请求ID - {} 批量获取用户详细信息失败", rid);
            return err_response(rid, "批量获取用户详细信息失败");
        }
        LOG_DEBUG("请求ID - {} 获取到的会话成员数量: {}", rid, chatSessionMember_list.size());
        //3. 填充响应
        for(const auto &member : chatSessionMember_list) {
            LOG_DEBUG("开始填充响应");
            auto chat_session_member_item = response->add_member_list();
            chat_session_member_item->set_role(static_cast<ChatSessionMemberRole>(member.role));
            chat_session_member_item->set_join_time(boost::posix_time::to_time_t(member.join_time));
            auto user_info = chat_session_member_item->mutable_user_info();
            user_info->set_user_id(member.user_id);
            user_info->set_nickname(user_list[member.user_id].nickname());
            user_info->set_description(user_list[member.user_id].description());
            user_info->set_description(user_list[member.user_id].mail());
            user_info->set_description(user_list[member.user_id].avatar());
        }
        response->set_request_id(rid);
        response->set_success(true);
    }
    /* brief: 设置聊天会话名称 */
    virtual void SetChatSessionName(::google::protobuf::RpcController* controller,
                        const ::chatnow::SetChatSessionNameReq* request,
                        ::chatnow::SetChatSessionNameRsp* response,
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
        std::string rid  = request->request_id();
        std::string uid  = request->user_id();
        std::string ssid = request->chat_session_id();
        std::string cssName = request->chat_session_name();
        //2. 对用户鉴权
        //2.1 取出用户在会话的信息
        auto csm = _mysql_chat_session_member->select(ssid, uid);
        if(!csm) {
            LOG_ERROR("请求ID - {} 用户不在该会话中", rid);
            return err_response(rid, "用户不在该会话中");
        }
        //2.2 鉴权
        if(csm->role() == ChatSessionRole::NORMAL) {
            LOG_ERROR("请求ID - {} 该用户 {} 没有权限修改会话名称", rid, uid);
            return err_response(rid, "该用户没有权限修改会话名称");
        }
        //3. 取出要修改的会话
        auto chatSession = _mysql_chat_session->select(ssid);
        if(!chatSession) {
            LOG_ERROR("请求ID - {} 要修改的会话 {} 不存在", rid, ssid);
            return err_response(rid, "要修改的会话不存在");
        }
        //4. 修改会话名称并更新数据库数据
        chatSession->chat_session_name(cssName);
        bool ret = _mysql_chat_session->update(chatSession);
        if(ret == false) {
            LOG_ERROR("请求ID - {} 更新数据库数据失败");
            return err_response(rid, "更新数据库数据失败");
        }
        //4.1 更新ES数据
        ret = _es_chat_session->append_data(*chatSession);
        if(ret == false) {
            LOG_ERROR("请求ID: {} - 更新 ES搜索引擎 会话名称失败: {}", rid, cssName);
            return err_response(request->request_id(), "更新 ES搜索引擎 会话名称失败");                      
        }
        //5. 填充响应
        response->set_request_id(rid);
        response->set_success(true);
    }
    /* brief: 设置聊天会话头像 */
    virtual void SetChatSessionAvatar(::google::protobuf::RpcController* controller,
                        const ::chatnow::SetChatSessionAvatarReq* request,
                        ::chatnow::SetChatSessionAvatarRsp* response,
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
        std::string rid  = request->request_id();
        std::string uid  = request->user_id();
        std::string ssid = request->chat_session_id();
        //2. 对用户鉴权
        //2.1 取出用户在会话的信息
        auto csm = _mysql_chat_session_member->select(ssid, uid);
        if(!csm) {
            LOG_ERROR("请求ID - {} 用户 {} 不在会话 {} 中", rid, uid, ssid);
            return err_response(rid, "用户不在该会话中");
        }
        //2.2 鉴权
        if(csm->role() == ChatSessionRole::NORMAL) {
            LOG_ERROR("请求ID - {} 该用户 {} 没有权限修改会话头像", rid, uid);
            return err_response(rid, "该用户没有权限修改会话头像");
        }
        //3. 取出要修改的对话 
        auto chatSession = _mysql_chat_session->select(ssid);
        if(!chatSession) {
            LOG_ERROR("请求ID - {} 要修改的会话 {} 不存在", rid, ssid);
            return err_response(rid, "要修改的会话不存在");
        }
        //注：检查会话类型，如果是单聊就不能改
        if(chatSession->chat_session_type() == ChatSessionType::SINGLE) {
            LOG_ERROR("请求ID - {} 会话类型为单聊，不能修改会话头像");
            return err_response(rid, "会话类型为单聊，不能修改会话头像");
        }
        //4. 上传头像文件到文件存储子服务
        std::string new_avatar_id;
        bool ret = PutSingleFile(rid, request->avatar(), new_avatar_id);
        if(ret == false) {
            LOG_ERROR("请求ID - {} 上传群聊头像失败");
            return err_response(rid, "上传群聊头像失败");
        }
        //5. 更新数据库中的头像ID
        chatSession->avatar_id(new_avatar_id);
        ret = _mysql_chat_session->update(chatSession);
        if(ret == false) {
            LOG_ERROR("请求ID - {} 更新数据库中的头像ID失败");
            return err_response(rid, "更新数据库中的头像ID失败");
        }
        //5.1 更新ES数据
        ret = _es_chat_session->append_data(*chatSession);
        if(ret == false) {
            LOG_ERROR("请求ID: {} - 更新 ES搜索引擎 会话头像ID失败: {}", rid, new_avatar_id);
            return err_response(request->request_id(), "更新 ES搜索引擎 会话头像ID失败");                      
        }
        //6. 填充响应
        response->set_request_id(rid);
        response->set_success(true);
    }
    /* brief: 新增群聊会话成员 */
    virtual void AddChatSessionMember(::google::protobuf::RpcController* controller,
                        const ::chatnow::AddChatSessionMemberReq* request,
                        ::chatnow::AddChatSessionMemberRsp* response,
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
        std::string rid  = request->request_id();
        std::string uid  = request->user_id();
        std::string ssid = request->chat_session_id();
        //2. 对用户鉴权
        //2.1 取出用户在会话的信息
        auto csm = _mysql_chat_session_member->select(ssid, uid);
        if(!csm) {
            LOG_ERROR("请求ID - {} 用户不在该会话中", rid);
            return err_response(rid, "用户不在该会话中");
        }
        //2.2 鉴权
        if(csm->role() == ChatSessionRole::NORMAL) {
            LOG_ERROR("请求ID - {} 该用户 {} 没有权限新增会话成员", rid, uid);
            return err_response(rid, "该用户没有权限新增会话成员");
        }
        //3. 新增会话成员到数据库
        std::vector<ChatSessionMember> new_member_list;
        for(int i = 0; i < request->member_id_list_size(); ++i) {
            ChatSessionMember member(ssid, 
                                    request->member_id_list(i),
                                    false,
                                    true,
                                    ChatSessionRole::NORMAL,
                                    boost::posix_time::second_clock::local_time());
            new_member_list.push_back(member);
        }
        bool ret = _mysql_chat_session_member->append(new_member_list);
        if(ret == false) {
            LOG_ERROR("请求ID - {} 向数据库批量添加会话成员失败", rid);
            return err_response(rid, "向数据库批量添加会话成员失败");
        }
        //4. 填充响应
        response->set_request_id(rid);
        response->set_success(true);
    }
    /* brief: 移除会话成员 */
    virtual void RemoveChatSessionMember(::google::protobuf::RpcController* controller,
                        const ::chatnow::RemoveChatSessionMemberReq* request,
                        ::chatnow::RemoveChatSessionMemberRsp* response,
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
        std::string rid  = request->request_id();
        std::string uid  = request->user_id();
        std::string ssid = request->chat_session_id();
        //2. 对用户鉴权
        //2.1 取出用户在会话的信息
        auto csm = _mysql_chat_session_member->select(ssid, uid);
        if(!csm) {
            LOG_ERROR("请求ID - {} 用户不在该会话中", rid);
            return err_response(rid, "用户不在该会话中");
        }
        //2.2 鉴权
        if(csm->role() == ChatSessionRole::NORMAL) {
            LOG_ERROR("请求ID - {} 该用户 {} 没有权限移除会话成员", rid, uid);
            return err_response(rid, "该用户没有权限移除会话成员");
        }
        //3. 移除数据库中会话成员
        //3.1 查询出要删除的会话成员 
        std::vector<std::string> members;
        for(const auto &e : request->member_id_list()) {
            members.push_back(e);
        }
        std::vector<ChatSessionMember> member_list = _mysql_chat_session_member->select(ssid, members);
        //3.2 移除数据库中的数据
        bool ret = _mysql_chat_session_member->remove(member_list);
        if(ret == false) {
            LOG_ERROR("请求ID - {} 移除数据库数据的成员数据失败", rid);
            return err_response(rid, "移除数据库数据的成员数据失败");
        }

        //4. 填充响应
        response->set_request_id(rid);
        response->set_success(true);
    }
    /* brief: 转让群聊群主 */
    virtual void TransferChatSessionOwner(::google::protobuf::RpcController* controller,
                        const ::chatnow::TransferChatSessionOwnerReq* request,
                        ::chatnow::TransferChatSessionOwnerRsp* response,
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
        std::string rid  = request->request_id();
        std::string uid  = request->user_id();
        std::string ssid = request->chat_session_id();
        std::string new_owner_id = request->new_owner_id();
        //2. 对操作鉴权
        //2.1 取出用户在会话的信息
        auto csm = _mysql_chat_session_member->select(ssid, uid);
        if(!csm) {
            LOG_ERROR("请求ID - {} 用户不在该会话中", rid);
            return err_response(rid, "用户不在该会话中");
        }
        //2.2 鉴权
        if(csm->role() != ChatSessionRole::OWNER) {
            LOG_ERROR("请求ID - {} 该用户 {} 没有权限转让群主", rid, uid);
            return err_response(rid, "该用户没有权限转让群主");
        }
        //3. 取出新群主在会话的信息
        auto newcsm = _mysql_chat_session_member->select(ssid, new_owner_id);
        if(!newcsm) {
            LOG_ERROR("请求ID - {} 新群主不在该会话中", rid);
            return err_response(rid, "新群主不在该会话中");
        }
        //4. 进行权限转换
        csm->role(ChatSessionRole::ADMIN);
        newcsm->role(ChatSessionRole::OWNER);
        //5. 同步数据到数据库
        bool ret = _mysql_chat_session_member->update({csm, newcsm});
        if(ret == false) {
            LOG_ERROR("请求ID - {} 同步转让数据到数据库失败");
            return err_response(rid, "同步转让数据到数据库失败");
        }
        //6. 组织响应
        response->set_request_id(rid);
        response->set_success(true);
    }
    /* brief: 编辑群聊成员权限 */
    virtual void ModifyMemberPermission(::google::protobuf::RpcController* controller,
                        const ::chatnow::ModifyMemberPermissionReq* request,
                        ::chatnow::ModifyMemberPermissionRsp* response,
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
        std::string rid  = request->request_id();
        std::string uid  = request->user_id();
        std::string ssid = request->chat_session_id();
        std::string cuid = request->changed_user_id();
        ChatSessionRole role = static_cast<ChatSessionRole>(request->role());
        //2. 对操作鉴权
        //2.1 取出要编辑用户权限的用户在会话的信息
        auto modifyer = _mysql_chat_session_member->select(ssid, uid);
        if(!modifyer) {
            LOG_ERROR("请求ID - {} 用户 {} 不在该会话中", rid, uid);
            return err_response(rid, "用户不在该会话中");
        }
        //2.2 鉴权
        if(modifyer->role() == ChatSessionRole::NORMAL) {
            LOG_ERROR("请求ID - {} 该用户 {} 没有权限更改群员权限", rid, uid);
            return err_response(rid, "该用户没有权限更改群员权限");
        }
        //3. 取出被编辑的用户
        auto csm = _mysql_chat_session_member->select(ssid, cuid);
        if(!csm) {
            LOG_ERROR("请求ID - {} 用户 {} 不在该会话中", rid, cuid);
            return err_response(rid, "用户不在该会话中");
        }
        //4. 对被编辑的用户鉴权，看看当前用户有没有权限更改他的权限
        if(csm->role() == ChatSessionRole::NORMAL) {
            //3.1.1 目标用户是普通群员，只要通过了第2步鉴权，随意改
            csm->role(role);
        } else if(csm->role() == ChatSessionRole::ADMIN) {
            //3.1.2 目标用户是管理员，只有群主能改
            if(modifyer->role() == ChatSessionRole::OWNER) {
                csm->role(role);
            } else {
                LOG_ERROR("请求ID - {} 用户没有权限更改管理员 {} 的权限", rid, uid, cuid);
                return err_response(rid, "当前用户没有权限更改目标用户的权限");
            }
        } else {
            //3.1.3 目标用户是群主，反了天了
            LOG_ERROR("请求ID - {} 用户 {} 没有权限更改用户 {} 的权限", rid, uid, cuid);
            return err_response(rid, "当前用户没有权限更改目标用户的权限");
        }
        //5. 同步更改到数据库
        bool ret = _mysql_chat_session_member->update(csm);
        if(ret == false) {
            LOG_ERROR("请求ID - {} 同步权限更改操作到数据库失败", rid);
            return err_response(rid, "同步权限更改操作到数据库失败");
        }
        //6. 填充响应
        response->set_request_id(rid);
        response->set_success(true);
    }
    /* brief: 编辑群聊会话状态 */
    virtual void ModifyChatSessionStatus(::google::protobuf::RpcController* controller,
                        const ::chatnow::ModifyChatSessionStatusReq* request,
                        ::chatnow::ModifyChatSessionStatusRsp* response,
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
        std::string rid  = request->request_id();
        std::string uid  = request->user_id();
        std::string ssid = request->chat_session_id();
        int role = static_cast<int>(request->status());
        //2. 对操作鉴权
        //2.1 取出要编辑用户权限的用户在会话的信息
        auto modifyer = _mysql_chat_session_member->select(ssid, uid);
        if(!modifyer) {
            LOG_ERROR("请求ID - {} 用户 {} 不在该会话中", rid, uid);
            return err_response(rid, "用户不在该会话中");
        }
        //2.2 鉴权
        if(modifyer->role() != ChatSessionRole::OWNER) {
            LOG_ERROR("请求ID - {} 该用户 {} 没有权限更改群聊状态", rid, uid);
            return err_response(rid, "该用户没有权限更改群聊状态");
        }
        //3. 更改会话状态
        //3.1 取出会话详细信息
        auto chatSession = _mysql_chat_session->select(ssid);
        if(!chatSession) {
            LOG_ERROR("请求ID - {} 需要查询的会话 {} 信息不存在", rid, ssid);
            return err_response(rid, "需要查询的会话信息不存在");
        }
        //3.2 更改会话状态
        chatSession->status(role);
        //4. 同步更改到数据库
        bool ret = _mysql_chat_session->update(chatSession);
        if(ret == false) {
            LOG_ERROR("请求ID - {} 同步更改会话 {} 状态信息到数据失败", rid, ssid);
            return err_response(rid, "同步更改会话状态信息到数据失败");
        }
        //4.1 更新ES数据
        ret = _es_chat_session->append_data(*chatSession);
        if(ret == false) {
            LOG_ERROR("请求ID: {} - 更新 ES搜索引擎 会话状态失败", rid);
            return err_response(request->request_id(), "更新 ES搜索引擎 会话状态失败");                      
        }
        //5. 填充响应
        response->set_request_id(rid);
        response->set_success(true);
    }
    virtual void SearchChatSession(::google::protobuf::RpcController* controller,
                        const ::chatnow::SearchChatSessionReq* request,
                        ::chatnow::SearchChatSessionRsp* response,
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
        std::string key = request->search_key(); 
        //2. 构建搜索条件
        std::optional<ChatSessionType> type = std::nullopt;
        if (request->has_session_type()) {
            type = static_cast<ChatSessionType>(request->session_type());
        }
        // 3. 调用 ES 搜索
        auto search_res = _es_chat_session->search(key, type);
        std::vector<std::string> ssid_list;
        for (auto &ssid : search_res) {
            ssid_list.push_back(ssid);
        }
        //3. 根据搜索出的会话ID列表取数据库取
        auto chat_session_list = _mysql_chat_session->select(ssid_list);
        //4. 组织响应
        for(const auto &chat_session : chat_session_list) {
            auto chat_session_info = response->add_chat_session_list();
            chat_session_info->set_chat_session_id(chat_session.chat_session_id());
            chat_session_info->set_chat_session_name(chat_session.chat_session_name());
            chat_session_info->set_avatar(chat_session.avatar_id());
            chat_session_info->set_chat_session_type(static_cast<int>(chat_session.chat_session_type()));
            chat_session_info->set_create_time(boost::posix_time::to_time_t(chat_session.create_time()));
            chat_session_info->set_member_count(chat_session.member_count());
        }
        response->set_request_id(rid);
        response->set_success(true);
    }
    //====================================================================================//
    //========================== 群员更改自身针对群聊的配置操作 =============================//
    //====================================================================================//
    /* brief: 用户设置自己在指定群聊的免打扰状态 */
    virtual void SetSessionMuted(::google::protobuf::RpcController* controller,
                        const ::chatnow::SetSessionMutedReq* request,
                        ::chatnow::SetSessionMutedRsp* response,
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
        std::string rid  = request->request_id();
        std::string uid  = request->user_id();
        std::string ssid = request->chat_session_id();
        //2. 取出用户在会话中的信息
        auto csm = _mysql_chat_session_member->select(ssid, uid);
        if(!csm) {
            LOG_ERROR("请求ID - {} 查询用户 {} 在会话 {} 中的信息失败", rid, uid);
            return err_response(rid, "查询用户在会话中的信息失败");
        }
        //3. 更改免打扰状态并同步到数据库
        csm->muted(request->is_muted());
        bool ret = _mysql_chat_session_member->update(csm);
        if(ret == false) {
            LOG_ERROR("请求ID - {} 用户 {} 更改在会话 {} 中的免打扰状态失败");
            return err_response(rid, "用户更改在会话中的免打扰状态失败");
        }
        //4. 组织响应
        response->set_request_id(rid);
        response->set_success(true);
    }
    /* brief: 用户设置是否开启指定群聊的免打扰 */
    virtual void SetSessionPinned(::google::protobuf::RpcController* controller,
                        const ::chatnow::SetSessionPinnedReq* request,
                        ::chatnow::SetSessionPinnedRsp* response,
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
        std::string rid  = request->request_id();
        std::string uid  = request->user_id();
        std::string ssid = request->chat_session_id();
        bool is_pinned = request->is_pinned();
        //2. 取出用户在会话中的信息
        auto csm = _mysql_chat_session_member->select(ssid, uid);
        if(!csm) {
            LOG_ERROR("请求ID - {} 查询用户 {} 在会话 {} 中的信息失败", rid, uid);
            return err_response(rid, "查询用户在会话中的信息失败");
        }
        //3. 更改置顶状态
        if(is_pinned == true) {
            //3.1 填充置顶时间表示开启置顶
            csm->pin_time(boost::posix_time::second_clock::local_time());
        } else {
            //3.2 将置顶时间字段置为空
            csm->unpin();
        }
        bool ret = _mysql_chat_session_member->update(csm);
        if(ret == false) {
            LOG_ERROR("请求ID - {} 更改置顶状态失败");
            return err_response(rid, "更改置顶状态失败");
        }
        //4. 组织响应
        response->set_request_id(rid);
        response->set_success(true);
    }
    /* brief: 设置会话可见状态 */
    virtual void SetSessionVisible(::google::protobuf::RpcController* controller,
                        const ::chatnow::SetSessionVisibleReq* request,
                        ::chatnow::SetSessionVisibleRsp* response,
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
        std::string rid  = request->request_id();
        std::string uid  = request->user_id();
        std::string ssid = request->chat_session_id();
        //2. 取出用户在会话中的信息
        auto csm = _mysql_chat_session_member->select(ssid, uid);
        if(!csm) {
            LOG_ERROR("请求ID - {} 查询用户 {} 在会话 {} 中的信息失败", rid, uid);
            return err_response(rid, "查询用户在会话中的信息失败");
        }
        //3. 更改隐藏/显示状态
        csm->visible(request->is_visible());
        bool ret = _mysql_chat_session_member->update(csm);
        if(ret == false) {
            LOG_ERROR("请求ID - {} 更改置顶状态失败");
            return err_response(rid, "更改置顶状态失败");
        }
        //4. 组织响应
        response->set_request_id(rid);
        response->set_success(true);
    }
    /* brief: 获取用户在会话的个人状态 */
    virtual void GetUserSessionStatus(::google::protobuf::RpcController* controller,
                        const ::chatnow::GetUserSessionStatusReq* request,
                        ::chatnow::GetUserSessionStatusRsp* response,
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
        std::string rid  = request->request_id();
        std::string uid  = request->user_id();
        std::string ssid = request->chat_session_id();
        //2. 取出用户在会话中的信息
        auto csm = _mysql_chat_session_member->select(ssid, uid);
        if(!csm) {
            LOG_ERROR("请求ID - {} 查询用户 {} 在会话 {} 中的信息失败", rid, uid);
            return err_response(rid, "查询用户在会话中的信息失败");
        }
        //3. 组织响应
        auto member = response->mutable_chat_session_member_info();
        member->set_chat_session_id(csm->session_id());
        member->set_user_id(uid);
        if(csm->last_read_msg() != 0) {
            member->set_last_message_id(csm->last_read_msg());
        }
        member->set_is_muted(csm->muted());
        member->set_is_visible(csm->visible());
        if(csm->pin_time().is_not_a_date_time()) {
            member->set_pin_time(boost::posix_time::to_time_t(csm->pin_time()));
        }
        member->set_role(static_cast<ChatSessionMemberRole>(csm->role()));
        member->set_join_time(boost::posix_time::to_time_t(csm->join_time()));
        response->set_request_id(rid);
        response->set_success(true);
    }
    /* brief: 退出会话 */
    virtual void QuitChatSession(::google::protobuf::RpcController* controller,
                        const ::chatnow::QuitChatSessionReq* request,
                        ::chatnow::QuitChatSessionRsp* response,
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
        std::string rid  = request->request_id();
        std::string uid  = request->user_id();
        std::string ssid = request->chat_session_id();
        //2. 取出用户在会话中的信息
        auto csm = _mysql_chat_session_member->select(ssid, uid);
        if(!csm) {
            LOG_ERROR("请求ID - {} 查询用户 {} 在会话 {} 中的信息失败", rid, uid);
            return err_response(rid, "查询用户在会话中的信息失败");
        }
        //3. 鉴权，如果是群主则必须先转移群主
        if(csm->role() == ChatSessionRole::OWNER) {
            LOG_ERROR("请求ID - {} 该用户 {} 必须先转让群主", rid, uid);
            return err_response(rid, "该用户必须先转让群主");
        }
        //4. 移除用户在数据库的信息
        bool ret = _mysql_chat_session_member->remove(*csm);
        if(ret == false) {
            LOG_ERROR("请求ID - {} 用户 {} 退出会话 {} 失败", rid, uid, ssid);
            return err_response(rid, "用户退出会话失败");
        }
        //5. 组织响应
        response->set_request_id(rid);
        response->set_success(true);
    }
    /* brief: 消息已读确认 */
    virtual void MsgReadAck(google::protobuf::RpcController* controller,
                            const ::chatnow::MsgReadAckReq* request,
                            ::chatnow::MsgReadAckRsp* response,
                            ::google::protobuf::Closure* done)
    {
        brpc::ClosureGuard rpc_guard(done);
        auto err_response = [this, response](const std::string &rid, const std::string &err_msg) -> void {
            response->set_request_id(rid);
            response->set_success(false);
            response->set_errmsg(err_msg);
            return;
        };
        std::string rid = request->request_id();
        std::string uid = request->user_id();
        std::string ssid = request->chat_session_id();
        unsigned long msg_id = request->message_id();

        // 执行数据库更新
        bool ret = _mysql_chat_session_member->update_last_read_msg(uid, ssid, msg_id);
        if (ret == false) {
            LOG_ERROR("请求ID - {} 更新会话成员 {} 已读消息ID失败", rid, uid);
            err_response(rid, "更新会话成员已读消息ID失败");
        }
        LOG_INFO("REQ: {} - 用户 {} 在会话 {} 已读至 {}", rid, uid, ssid, msg_id);
        response->set_request_id(rid);
        response->set_success(true);
    }
    //============================================================================
    //============================== 内部接口 =====================================
    //============================================================================
    /* brief: 获取成员ID列表 */
    virtual void GetMemberIdList(::google::protobuf::RpcController* controller,
                       const ::chatnow::GetMemberIdListReq* request,
                       ::chatnow::GetMemberIdListRsp* response,
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
        std::string rid  = request->request_id();
        std::string ssid = request->chat_session_id();
        //2. 查询会话成员
        auto member_id_list = _mysql_chat_session_member->members(ssid);
        if(member_id_list.size() == 0) {
            LOG_ERROR("请求ID - {} 没有查询到会话 {} 成员", rid, ssid);
            return err_response(rid, "没有查询到会话成员");
        }
        //3. 组织响应
        for(const auto &member_id : member_id_list) {
            response->add_member_id_list(member_id);
        }
        response->set_request_id(rid);
        response->set_success(true);
    }

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
    /* brief: 上传文件到文件存储子服务 */
    bool PutSingleFile(const std::string &rid,
                    const std::string &file_data,
                    std::string &file_id)
    {
        auto channel = _mm_channels->choose(_file_service_name);
        if(!channel) {
            LOG_ERROR("请求ID: {} - 未找到文件子服务: {}", rid, _file_service_name);
            return false;
        }
        FileService_Stub stub(channel.get());
        PutSingleFileReq req;
        PutSingleFileRsp rsp;
        req.set_request_id(rid);
        req.mutable_file_data()->set_file_name("");
        req.mutable_file_data()->set_file_size(file_data.size());
        req.mutable_file_data()->set_file_content(file_data);
        brpc::Controller cntl;
        stub.PutSingleFile(&cntl, &req, &rsp, nullptr);
        if(cntl.Failed() == true) {
            LOG_ERROR("请求ID {} - 文件存储子服务调用失败: {}", rid, cntl.ErrorText());
            return false;
        }
        if(rsp.success() == false) {
            LOG_ERROR("请求ID {} - 上传文件失败: {}", rsp.errmsg());
            return false;
        }
        file_id = rsp.file_info().file_id();
        return true;
    }
private:
    ESChatSession::ptr _es_chat_session;

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
                            const std::string &file_service_name,
                            const std::string &message_service_name)
    {
        _user_service_name = user_service_name;
        _file_service_name = file_service_name;
        _message_service_name = message_service_name;
        _mm_channels = std::make_shared<ServiceManager>();
        _mm_channels->declared(user_service_name);
        _mm_channels->declared(file_service_name);
        _mm_channels->declared(message_service_name);
        LOG_DEBUG("设置用户子服务为需添加管理的子服务: {}", _user_service_name);
        LOG_DEBUG("设置文件存储子服务为需添加管理的子服务: {}", _file_service_name);
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
        if(!_mysql_client) {
            LOG_ERROR("还未初始化MySQL数据库模块");
            abort();
        }
        if(!_mm_channels) {
            LOG_ERROR("还未初始化信道管理模块");
            abort();
        }

        ChatSessionServiceImpl *chatsession_service = new ChatSessionServiceImpl(_es_client, _mysql_client, _mm_channels, _user_service_name, _file_service_name, _message_service_name);
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

    std::shared_ptr<elasticlient::Client> _es_client;
    std::shared_ptr<odb::core::database> _mysql_client;

    ServiceManager::ptr _mm_channels;
    Discovery::ptr _service_discover;

    std::shared_ptr<brpc::Server> _rpc_server;
    std::string _user_service_name;
    std::string _file_service_name;
    std::string _message_service_name;
};

} // namespace chatnow;