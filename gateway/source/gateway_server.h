#pragma once

#include <brpc/server.h>
#include <butil/logging.h>

#include "httplib.h"

#include "dao/data_redis.hpp"
#include "infra/etcd.hpp"
#include "infra/logger.hpp"
#include "mq/channel.hpp"
#include "utils/brpc_closure.hpp"

// 注意：B3 — Gateway 已彻底无状态化，WebSocket 终结迁至 push 服务（push_server.h）。
// 不再 include "connection.hpp"，不再监听 9001 端口，不再维护 uid→conn 映射。

/* protobuf 框架代码 */
#include "common/types.pb.h"
#include "common/error.pb.h"
#include "common/envelope.pb.h"
#include "common/presence.pb.h"
#include "identity/identity_service.pb.h"
#include "relationship/relationship_service.pb.h"
#include "conversation/conversation_service.pb.h"
#include "message/message_types.pb.h"
#include "message/message_service.pb.h"
#include "media/media_service.pb.h"
#include "push/notify.pb.h"
#include "push/push_service.pb.h"
#include "transmite/transmite_service.pb.h"

namespace chatnow
{

#define GET_MAIL_VERIFY_CODE        "/service/user/get_mail_verify_code"
#define USERNAME_REGISTER           "/service/user/username_register"                 //用户名密码注册 
#define USERNAME_LOGIN              "/service/user/username_login"                    //用户名密码登录 
#define MAIL_REGISTER               "/service/user/mail_register"                     //邮箱号码注册 
#define MAIL_LOGIN                  "/service/user/mail_login"                        //邮箱号码登录 
#define GET_USER_INFO               "/service/user/get_user_info"                     //获取个人信息 
#define SET_AVATAR                  "/service/user/set_avatar"                        //修改头像 
#define SET_NICKNAME                "/service/user/set_nickname"                      //修改昵称 
#define SET_DESCRIPTION             "/service/user/set_description"                   //修改签名 
#define SET_MAIL                    "/service/user/set_mail"                          //修改绑定邮箱 
#define GET_FRIEND_LIST             "/service/friend/get_friend_list"                 //获取好友列表 
#define GET_FRIEND_INFO             "/service/friend/get_friend_info"                 //获取好友信息 
#define ADD_FRIEND_APPLY            "/service/friend/add_friend_apply"                //发送好友申请 
#define ADD_FRIEND_PROCESS          "/service/friend/add_friend_process"              //好友申请处理 
#define REMOVE_FRIEND               "/service/friend/remove_friend"                   //删除好友 
#define SEARCH_FRIEND               "/service/friend/search_friend"                   //搜索用户 
#define GET_CHAT_SESSION_LIST       "/service/chatsession/get_chat_session_list"      //获取指定用户的消息会话列表 
#define CREATE_CHAT_SESSION         "/service/chatsession/create_chat_session"        //创建消息会话 
#define GET_CHAT_SESSION_MEMBER     "/service/chatsession/get_chat_session_member"    //获取消息会话成员列表 
#define GET_PENDING_FRIEND_EVENTS   "/service/friend/get_pending_friend_events"       //获取待处理好友申请事件列表 
#define GET_HISTORY                 "/service/message_storage/get_history"            //获取历史消息/离线消息列表 
#define GET_RECENT                  "/service/message_storage/get_recent"             //获取最近 N 条消息列表 
#define SEARCH_HISTORY              "/service/message_storage/search_history"         //搜索历史消息 
#define NEW_MESSAGE                 "/service/message_transmit/new_message"           //发送消息 
#define GET_SINGLE_FILE             "/service/file/get_single_file"                   //获取单个文件数据 
#define GET_MULTI_FILE              "/service/file/get_multi_file"                    //获取多个文件数据 
#define PUT_SINGLE_FILE             "/service/file/put_single_file"                   //发送单个文件 
#define PUT_MULTI_FILE              "/service/file/put_multi_file"                    //发送多个文件 
#define RECOGNITION                 "/service/speech/recognition"                     //语音转文字
    
#define GET_CHAT_SESSION_DETAIL     "/service/chatsession/get_chat_session_detail"    //获取指定会话详细信息
#define SET_CHAT_SESSION_NAME       "/service/chatsession/set_chat_session_name"      //设置会话名称
#define SET_CHAT_SESSION_AVATAR     "/service/chatsession/set_chat_session_avatar"    //设置会话头像
#define ADD_CHAT_SESSION_MEMBER     "/service/chatsession/add_chat_session_member"    //添加会话成员
#define REMOVE_CHAT_SESSION_MEMBER  "/service/chatsession/remove_chat_session_member" //移除会话成员
#define TRANSFER_CHAT_SESSION_OWNER "/service/chatsession/transfer_chat_session_owner"//转让群主
#define MODIFY_MEMBER_PERMISSION    "/service/chatsession/modify_member_permission"   //编辑会话成员权限
#define MODIFY_CHAT_SESSION_STATUS  "/service/chatsession/modify_chat_session_status" //编辑会话状态
#define SEARCH_CHAT_SESSION         "/service/chatsession/search_chat_session"        //搜索聊天会话
#define SET_SESSION_MUTED           "/service/chatsession/set_session_muted"          //设置会话免打扰
#define SET_SESSION_PINNED          "/service/chatsession/set_session_pinned"         //设置会话置顶
#define SET_SESSION_VISIBLE         "/service/chatsession/set_session_visible"        //设置会话隐藏
#define GET_USER_SESSION_STATUS     "/service/chatsession/get_user_session_status"    //获取用户会话状态(如是否开启置顶等)
#define QUIT_CHAT_SESSION           "/service/chatsession/quit_chat_session"          //退出聊天会话
#define MSG_READ_ACK                "/service/chatsession/msg_read_ack"               //更新已读消息ID
#define GET_MEMBER_ID_LIST          "/service/chatsession/get_member_id_list"         //获取会话成员ID列表
#define GET_OFFLINE_MSG             "/service/message_storage/get_offline_msg"        //从 timeline 拉取用户的增量消息
//#define GET_MSG_BY_IDS              "/service/message_storage/get_msg_by_ids"         //通过消息ID获取消息（内部接口）
//#define DELETE_TIMELINE_MSG         "/service/message_storage/delete_timeline_msg"    //删除用户自己的timeline里的消息
#define GET_UNREAD_COUNT            "/service/message_storage/get_unread_count"       //帮助会话服务算未读消息数量
 
class GatewayServer
{
public:
    using ptr = std::shared_ptr<GatewayServer>;

    GatewayServer(int http_port,
                const std::shared_ptr<sw::redis::Redis> &redis_client,
                const ServiceManager::ptr &channels,
                const Discovery::ptr &service_discoverer,
                const std::string &speech_service_name,
                const std::string &file_service_name,
                const std::string &user_service_name,
                const std::string &transmite_service_name,
                const std::string &message_service_name,
                const std::string &friend_service_name,
                const std::string &chatsession_service_name,
                const std::string &push_service_name = "/service/push_service")
        : _redis_session(std::make_shared<Session>(redis_client)),
        _mm_channels(channels),
        _service_discoverer(service_discoverer),
        _speech_service_name(speech_service_name),
        _file_service_name(file_service_name),
        _user_service_name(user_service_name),
        _transmite_service_name(transmite_service_name),
        _message_service_name(message_service_name),
        _friend_service_name(friend_service_name),
        _chatsession_service_name(chatsession_service_name),
        _push_service_name(push_service_name),
        _http_port(http_port)
    {
        // B3: Gateway 不再终结 WebSocket。WS 由 push 服务监听 9001。
        //     此处仅注册 HTTP 路由；推送链路统一走 _pushNotify → PushService。

        //2. 搭建http服务器
        _http_server.Post(GET_MAIL_VERIFY_CODE,         (httplib::Server::Handler)std::bind(&GatewayServer::GetMailVerifyCode,         this, std::placeholders::_1, std::placeholders::_2));
        _http_server.Post(USERNAME_REGISTER,            (httplib::Server::Handler)std::bind(&GatewayServer::UserRegister,              this, std::placeholders::_1, std::placeholders::_2));
        _http_server.Post(USERNAME_LOGIN,               (httplib::Server::Handler)std::bind(&GatewayServer::UserLogin,                 this, std::placeholders::_1, std::placeholders::_2));
        _http_server.Post(MAIL_REGISTER,                (httplib::Server::Handler)std::bind(&GatewayServer::MailRegister,              this, std::placeholders::_1, std::placeholders::_2));
        _http_server.Post(MAIL_LOGIN,                   (httplib::Server::Handler)std::bind(&GatewayServer::MailLogin,                 this, std::placeholders::_1, std::placeholders::_2));
        _http_server.Post(GET_USER_INFO,                (httplib::Server::Handler)std::bind(&GatewayServer::GetUserInfo,               this, std::placeholders::_1, std::placeholders::_2));
        _http_server.Post(SET_AVATAR,                   (httplib::Server::Handler)std::bind(&GatewayServer::SetUserAvatar,             this, std::placeholders::_1, std::placeholders::_2));
        _http_server.Post(SET_NICKNAME,                 (httplib::Server::Handler)std::bind(&GatewayServer::SetUserNickname,           this, std::placeholders::_1, std::placeholders::_2));
        _http_server.Post(SET_DESCRIPTION,              (httplib::Server::Handler)std::bind(&GatewayServer::SetUserDescription,        this, std::placeholders::_1, std::placeholders::_2));
        _http_server.Post(SET_MAIL,                     (httplib::Server::Handler)std::bind(&GatewayServer::SetUserMailNumber,         this, std::placeholders::_1, std::placeholders::_2));
        _http_server.Post(GET_FRIEND_LIST,              (httplib::Server::Handler)std::bind(&GatewayServer::GetFriendList,             this, std::placeholders::_1, std::placeholders::_2));
        _http_server.Post(ADD_FRIEND_APPLY,             (httplib::Server::Handler)std::bind(&GatewayServer::FriendAdd,                 this, std::placeholders::_1, std::placeholders::_2));
        _http_server.Post(ADD_FRIEND_PROCESS,           (httplib::Server::Handler)std::bind(&GatewayServer::FriendAddProcess,          this, std::placeholders::_1, std::placeholders::_2));
        _http_server.Post(REMOVE_FRIEND,                (httplib::Server::Handler)std::bind(&GatewayServer::FriendRemove,              this, std::placeholders::_1, std::placeholders::_2));
        _http_server.Post(SEARCH_FRIEND,                (httplib::Server::Handler)std::bind(&GatewayServer::FriendSearch,              this, std::placeholders::_1, std::placeholders::_2));
        _http_server.Post(GET_CHAT_SESSION_LIST,        (httplib::Server::Handler)std::bind(&GatewayServer::GetChatSessionList,        this, std::placeholders::_1, std::placeholders::_2));
        _http_server.Post(CREATE_CHAT_SESSION,          (httplib::Server::Handler)std::bind(&GatewayServer::ChatSessionCreate,         this, std::placeholders::_1, std::placeholders::_2));
        _http_server.Post(GET_CHAT_SESSION_MEMBER,      (httplib::Server::Handler)std::bind(&GatewayServer::GetChatSessionMember,      this, std::placeholders::_1, std::placeholders::_2));
        _http_server.Post(GET_PENDING_FRIEND_EVENTS,    (httplib::Server::Handler)std::bind(&GatewayServer::GetPendingFriendEventList, this, std::placeholders::_1, std::placeholders::_2));
        _http_server.Post(GET_HISTORY,                  (httplib::Server::Handler)std::bind(&GatewayServer::GetHistoryMsg,             this, std::placeholders::_1, std::placeholders::_2));
        _http_server.Post(GET_RECENT,                   (httplib::Server::Handler)std::bind(&GatewayServer::GetRecentMsg,              this, std::placeholders::_1, std::placeholders::_2));
        _http_server.Post(SEARCH_HISTORY,               (httplib::Server::Handler)std::bind(&GatewayServer::MsgSearch,                 this, std::placeholders::_1, std::placeholders::_2));
        _http_server.Post(NEW_MESSAGE,                  (httplib::Server::Handler)std::bind(&GatewayServer::NewMessage,                this, std::placeholders::_1, std::placeholders::_2));
        _http_server.Post(GET_SINGLE_FILE,              (httplib::Server::Handler)std::bind(&GatewayServer::GetSingleFile,             this, std::placeholders::_1, std::placeholders::_2));
        _http_server.Post(GET_MULTI_FILE,               (httplib::Server::Handler)std::bind(&GatewayServer::GetMultiFile,              this, std::placeholders::_1, std::placeholders::_2));
        _http_server.Post(PUT_SINGLE_FILE,              (httplib::Server::Handler)std::bind(&GatewayServer::PutSingleFile,             this, std::placeholders::_1, std::placeholders::_2));
        _http_server.Post(PUT_MULTI_FILE,               (httplib::Server::Handler)std::bind(&GatewayServer::PutMultiFile,              this, std::placeholders::_1, std::placeholders::_2));
        _http_server.Post(RECOGNITION,                  (httplib::Server::Handler)std::bind(&GatewayServer::SpeechRecognition,         this, std::placeholders::_1, std::placeholders::_2));
       
        _http_server.Post(GET_CHAT_SESSION_DETAIL,      (httplib::Server::Handler)std::bind(&GatewayServer::GetChatSessionDetail,      this, std::placeholders::_1, std::placeholders::_2));
        _http_server.Post(SET_CHAT_SESSION_NAME,        (httplib::Server::Handler)std::bind(&GatewayServer::SetChatSessionName,        this, std::placeholders::_1, std::placeholders::_2));
        _http_server.Post(SET_CHAT_SESSION_AVATAR,      (httplib::Server::Handler)std::bind(&GatewayServer::SetChatSessionAvatar,      this, std::placeholders::_1, std::placeholders::_2));
        _http_server.Post(ADD_CHAT_SESSION_MEMBER,      (httplib::Server::Handler)std::bind(&GatewayServer::AddChatSessionMember,     this, std::placeholders::_1, std::placeholders::_2));
        _http_server.Post(REMOVE_CHAT_SESSION_MEMBER,   (httplib::Server::Handler)std::bind(&GatewayServer::RemoveChatSessionMember,   this, std::placeholders::_1, std::placeholders::_2));
        _http_server.Post(TRANSFER_CHAT_SESSION_OWNER,  (httplib::Server::Handler)std::bind(&GatewayServer::TransferChatSessionOwner,  this, std::placeholders::_1, std::placeholders::_2));
        _http_server.Post(MODIFY_MEMBER_PERMISSION,     (httplib::Server::Handler)std::bind(&GatewayServer::ModifyMemberPermission,    this, std::placeholders::_1, std::placeholders::_2));
        _http_server.Post(MODIFY_CHAT_SESSION_STATUS,   (httplib::Server::Handler)std::bind(&GatewayServer::ModifyChatSessionStatus,   this, std::placeholders::_1, std::placeholders::_2));
        _http_server.Post(SEARCH_CHAT_SESSION,          (httplib::Server::Handler)std::bind(&GatewayServer::SearchChatSession,         this, std::placeholders::_1, std::placeholders::_2));
        _http_server.Post(SET_SESSION_MUTED,            (httplib::Server::Handler)std::bind(&GatewayServer::SetSessionMuted,           this, std::placeholders::_1, std::placeholders::_2));
        _http_server.Post(SET_SESSION_PINNED,           (httplib::Server::Handler)std::bind(&GatewayServer::SetSessionPinned,          this, std::placeholders::_1, std::placeholders::_2));
        _http_server.Post(SET_SESSION_VISIBLE,          (httplib::Server::Handler)std::bind(&GatewayServer::SetSessionVisible,         this, std::placeholders::_1, std::placeholders::_2));
        _http_server.Post(QUIT_CHAT_SESSION,            (httplib::Server::Handler)std::bind(&GatewayServer::QuitChatSession,           this, std::placeholders::_1, std::placeholders::_2));
        _http_server.Post(MSG_READ_ACK,                 (httplib::Server::Handler)std::bind(&GatewayServer::MsgReadAck,                this, std::placeholders::_1, std::placeholders::_2));
        _http_server.Post(GET_MEMBER_ID_LIST,           (httplib::Server::Handler)std::bind(&GatewayServer::GetMemberIdList,           this, std::placeholders::_1, std::placeholders::_2));
        _http_server.Post(GET_OFFLINE_MSG,              (httplib::Server::Handler)std::bind(&GatewayServer::GetOfflineMsg,             this, std::placeholders::_1, std::placeholders::_2));
        //_http_server.Post(GET_MSG_BY_IDS,               (httplib::Server::Handler)std::bind(&GatewayServer::GetMsgByIDs,               this, std::placeholders::_1, std::placeholders::_2));
       // _http_server.Post(DELETE_TIMELINE_MSG,          (httplib::Server::Handler)std::bind(&GatewayServer::DeleteTimelineMsg,         this, std::placeholders::_1, std::placeholders::_2));
        _http_server.Post(GET_UNREAD_COUNT,             (httplib::Server::Handler)std::bind(&GatewayServer::GetUnreadCount,            this, std::placeholders::_1, std::placeholders::_2));

    }
    /* 启动服务器：阻塞主线程在 HTTP 监听上
     * httplib::listen 失败（bind / listen 错误）返回 false；不检查会让进程静默 exit 0
     */
    void start() {
        LOG_INFO("Gateway 启动: http_port={} (WS 已迁至 push 服务)", _http_port);
        if(!_http_server.listen("0.0.0.0", _http_port)) {
            LOG_ERROR("HTTP 服务器监听失败 port={}", _http_port);
            abort();
        }
    }
private:
    /* brief: 通过 Push 服务下发 NotifyMessage（无状态化推送链路）
     *  - 异步 fire-and-forget；用 SelfDeleteRpcClosure 持有 cntl/req/rsp，
     *    避免栈对象在异步回调到达前已析构（DoNothing 旧实现的 UAF）
     */
    void _pushNotify(const std::string &target_uid, const NotifyMessage &notify,
                     const std::string &rid = std::string()) {
        auto channel = _mm_channels->choose(_push_service_name);
        if(!channel) {
            LOG_WARN("Push 服务节点不可用，通知未下发 uid={} type={}", target_uid, (int)notify.notify_type());
            return;
        }
        PushService_Stub stub(channel.get());
        auto *closure = new SelfDeleteRpcClosure<PushToUserReq, PushToUserRsp>();
        closure->req.set_request_id(rid);
        closure->req.set_user_id(target_uid);
        closure->req.mutable_notify()->CopyFrom(notify);
        std::string uid_copy = target_uid;
        closure->on_done = [uid_copy](brpc::Controller *c, const PushToUserRsp &) {
            if(c->Failed()) {
                LOG_WARN("PushToUser 失败 uid={}: {}", uid_copy, c->ErrorText());
            }
        };
        stub.PushToUser(&closure->cntl, &closure->req, &closure->rsp, closure);
    }
    // B3: WS 入口与连接管理已下沉到 push 服务（push_server.h 的 make_ws_object）。
    //     gateway 不再持有 onOpen / onClose / onMessage / keepAlive 句柄，
    //     也不再访问 _connections / _ws_server。客户端鉴权 / 心跳 / ACK 全部走 push。
    /* brief: 获取邮件验证码 */
    void GetMailVerifyCode(const httplib::Request &request, httplib::Response &response) {
        //1. 取出http请求正文，将正文进行反序列化
        MailVerifyCodeReq req;
        MailVerifyCodeRsp rsp;
        auto err_response = [&req, &rsp, &response](const std::string &errmsg) -> void {
            rsp.set_success(false);
            rsp.set_errmsg(errmsg);
            response.set_content(rsp.SerializeAsString(), "application/x-protbuf");
        };
        bool ret = req.ParseFromString(request.body);
        if(ret == false) {
            LOG_ERROR("获取邮件验证码请求正文反序列化失败");
            return err_response("获取邮件验证码请求正文反序列化失败");
        }
        //2. 将请求转发给用户子服务进行业务处理
        auto channel = _mm_channels->choose(_user_service_name);
        if(!channel) {
            LOG_ERROR("请求ID - {} 未找到可提供业务的用户子服务节点", req.request_id());
            return err_response("未找到可提供业务的用户子服务节点");
        }
        UserService_Stub stub(channel.get());
        brpc::Controller cntl;
        stub.GetMailVerifyCode(&cntl, &req, &rsp, nullptr);
        if(cntl.Failed()) {
            LOG_ERROR("请求ID - {} 用户子服务调用失败: {}", req.request_id(), cntl.ErrorText());
            return err_response("用户子服务调用失败");
        }
        //3. 得到用户子服务的响应后，将响应进行序列化作为http响应正文
        response.set_content(rsp.SerializeAsString(), "application/x-protbuf");
    }
    /* brief: 用户注册 */
    void UserRegister(const httplib::Request &request, httplib::Response &response) {
        //1. 取出http请求正文，将正文进行反序列化
        UserRegisterReq req;
        UserRegisterRsp rsp;
        auto err_response = [&req, &rsp, &response](const std::string &errmsg) -> void {
            rsp.set_success(false);
            rsp.set_errmsg(errmsg);
            response.set_content(rsp.SerializeAsString(), "application/x-protbuf");
        };
        bool ret = req.ParseFromString(request.body);
        if(ret == false) {
            LOG_ERROR("用户名注册请求正文反序列化失败");
            return err_response("用户名注册请求正文反序列化失败");
        }
        //2. 将请求转发给用户子服务进行业务处理
        auto channel = _mm_channels->choose(_user_service_name);
        if(!channel) {
            LOG_ERROR("请求ID - {} 未找到可提供业务的用户子服务节点", req.request_id());
            return err_response("未找到可提供业务的用户子服务节点");
        }
        UserService_Stub stub(channel.get());
        brpc::Controller cntl;
        stub.UserRegister(&cntl, &req, &rsp, nullptr);
        if(cntl.Failed()) {
            LOG_ERROR("请求ID - {} 用户子服务调用失败: {}", req.request_id(), cntl.ErrorText());
            return err_response("用户子服务调用失败");
        }
        //3. 得到用户子服务的响应后，将响应进行序列化作为http响应正文
        response.set_content(rsp.SerializeAsString(), "application/x-protbuf");
    }
    /* brief: 用户登录 */
    void UserLogin(const httplib::Request &request, httplib::Response &response) {
        //1. 取出http请求正文，将正文进行反序列化
        UserLoginReq req;
        UserLoginRsp rsp;
        auto err_response = [&req, &rsp, &response](const std::string &errmsg) -> void {
            rsp.set_success(false);
            rsp.set_errmsg(errmsg);
            response.set_content(rsp.SerializeAsString(), "application/x-protbuf");
        };
        bool ret = req.ParseFromString(request.body);
        if(ret == false) {
            LOG_ERROR("用户名登录请求正文反序列化失败");
            return err_response("用户名登录请求正文反序列化失败");
        }
        //2. 将请求转发给用户子服务进行业务处理
        auto channel = _mm_channels->choose(_user_service_name);
        if(!channel) {
            LOG_ERROR("请求ID - {} 未找到可提供业务的用户子服务节点", req.request_id());
            return err_response("未找到可提供业务的用户子服务节点");
        }
        UserService_Stub stub(channel.get());
        brpc::Controller cntl;
        stub.UserLogin(&cntl, &req, &rsp, nullptr);
        if(cntl.Failed()) {
            LOG_ERROR("请求ID - {} 用户子服务调用失败: {}", req.request_id(), cntl.ErrorText());
            return err_response("用户子服务调用失败");
        }
        //3. 得到用户子服务的响应后，将响应进行序列化作为http响应正文
        response.set_content(rsp.SerializeAsString(), "application/x-protbuf");
    }
    void MailRegister(const httplib::Request &request, httplib::Response &response) {
        //1. 取出http请求正文，将正文进行反序列化
        MailRegisterReq req;
        MailRegisterRsp rsp;
        auto err_response = [&req, &rsp, &response](const std::string &errmsg) -> void {
            rsp.set_success(false);
            rsp.set_errmsg(errmsg);
            response.set_content(rsp.SerializeAsString(), "application/x-protbuf");
        };
        bool ret = req.ParseFromString(request.body);
        if(ret == false) {
            LOG_ERROR("邮箱注册请求正文反序列化失败");
            return err_response("邮箱注册请求正文反序列化失败");
        }
        //2. 将请求转发给用户子服务进行业务处理
        auto channel = _mm_channels->choose(_user_service_name);
        if(!channel) {
            LOG_ERROR("请求ID - {} 未找到可提供业务的用户子服务节点", req.request_id());
            return err_response("未找到可提供业务的用户子服务节点");
        }
        UserService_Stub stub(channel.get());
        brpc::Controller cntl;
        stub.MailRegister(&cntl, &req, &rsp, nullptr);
        if(cntl.Failed()) {
            LOG_ERROR("请求ID - {} 用户子服务调用失败: {}", req.request_id(), cntl.ErrorText());
            return err_response("用户子服务调用失败");
        }
        //3. 得到用户子服务的响应后，将响应进行序列化作为http响应正文
        response.set_content(rsp.SerializeAsString(), "application/x-protbuf");
    }
    void MailLogin(const httplib::Request &request, httplib::Response &response) {
        //1. 取出http请求正文，将正文进行反序列化
        MailLoginReq req;
        MailLoginRsp rsp;
        auto err_response = [&req, &rsp, &response](const std::string &errmsg) -> void {
            rsp.set_success(false);
            rsp.set_errmsg(errmsg);
            response.set_content(rsp.SerializeAsString(), "application/x-protbuf");
        };
        bool ret = req.ParseFromString(request.body);
        if(ret == false) {
            LOG_ERROR("邮箱登录请求正文反序列化失败");
            return err_response("邮箱登录请求正文反序列化失败");
        }
        //2. 将请求转发给用户子服务进行业务处理
        auto channel = _mm_channels->choose(_user_service_name);
        if(!channel) {
            LOG_ERROR("请求ID - {} 未找到可提供业务的用户子服务节点", req.request_id());
            return err_response("未找到可提供业务的用户子服务节点");
        }
        UserService_Stub stub(channel.get());
        brpc::Controller cntl;
        stub.MailLogin(&cntl, &req, &rsp, nullptr);
        if(cntl.Failed()) {
            LOG_ERROR("请求ID - {} 用户子服务调用失败: {}", req.request_id(), cntl.ErrorText());
            return err_response("用户子服务调用失败");
        }
        //3. 得到用户子服务的响应后，将响应进行序列化作为http响应正文
        response.set_content(rsp.SerializeAsString(), "application/x-protbuf");
    }
    void GetUserInfo(const httplib::Request &request, httplib::Response &response) {
        //1. 取出http请求正文，将正文进行反序列化
        GetUserInfoReq req;
        GetUserInfoRsp rsp;
        auto err_response = [&req, &rsp, &response](const std::string &errmsg) -> void {
            rsp.set_success(false);
            rsp.set_errmsg(errmsg);
            response.set_content(rsp.SerializeAsString(), "application/x-protbuf");
        };
        bool ret = req.ParseFromString(request.body);
        if(ret == false) {
            LOG_ERROR("获取用户信息请求正文反序列化失败");
            return err_response("获取用户信息请求正文反序列化失败");
        }
        //2. 客户端身份识别与鉴权
        std::string ssid = req.session_id();
        auto uid = _redis_session->uid(ssid);
        if(!uid) {
            LOG_ERROR("请求ID - {} 获取登录会话 {} 关联用户信息失败", req.request_id(), ssid);
            return err_response("获取登录会话关联用户信息失败");
        }
        req.set_user_id(*uid);
        //3. 将请求转发给用户子服务进行业务处理
        auto channel = _mm_channels->choose(_user_service_name);
        if(!channel) {
            LOG_ERROR("请求ID - {} 未找到可提供业务的用户子服务节点", req.request_id());
            return err_response("未找到可提供业务的用户子服务节点");
        }
        UserService_Stub stub(channel.get());
        brpc::Controller cntl;
        stub.GetUserInfo(&cntl, &req, &rsp, nullptr);
        if(cntl.Failed()) {
            LOG_ERROR("请求ID - {} 用户子服务调用失败: {}", req.request_id(), cntl.ErrorText());
            return err_response("用户子服务调用失败");
        }
        //4. 得到用户子服务的响应后，将响应进行序列化作为http响应正文
        response.set_content(rsp.SerializeAsString(), "application/x-protbuf");
    }
    void SetUserAvatar(const httplib::Request &request, httplib::Response &response) {
        //1. 取出http请求正文，将正文进行反序列化
        SetUserAvatarReq req;
        SetUserAvatarRsp rsp;
        auto err_response = [&req, &rsp, &response](const std::string &errmsg) -> void {
            rsp.set_success(false);
            rsp.set_errmsg(errmsg);
            response.set_content(rsp.SerializeAsString(), "application/x-protbuf");
        };
        bool ret = req.ParseFromString(request.body);
        if(ret == false) {
            LOG_ERROR("设置用户头像请求正文反序列化失败");
            return err_response("设置用户头像请求正文反序列化失败");
        }
        //2. 客户端身份识别与鉴权
        std::string ssid = req.session_id();
        auto uid = _redis_session->uid(ssid);
        if(!uid) {
            LOG_ERROR("请求ID - {} 获取登录会话 {} 关联用户信息失败", req.request_id(), ssid);
            return err_response("获取登录会话关联用户信息失败");
        }
        req.set_user_id(*uid);
        //3. 将请求转发给用户子服务进行业务处理
        auto channel = _mm_channels->choose(_user_service_name);
        if(!channel) {
            LOG_ERROR("请求ID - {} 未找到可提供业务的用户子服务节点", req.request_id());
            return err_response("未找到可提供业务的用户子服务节点");
        }
        UserService_Stub stub(channel.get());
        brpc::Controller cntl;
        stub.SetUserAvatar(&cntl, &req, &rsp, nullptr);
        if(cntl.Failed()) {
            LOG_ERROR("请求ID - {} 用户子服务调用失败: {}", req.request_id(), cntl.ErrorText());
            return err_response("用户子服务调用失败");
        }
        //4. 得到用户子服务的响应后，将响应进行序列化作为http响应正文
        response.set_content(rsp.SerializeAsString(), "application/x-protbuf");
    }
    void SetUserNickname(const httplib::Request &request, httplib::Response &response) {
        //1. 取出http请求正文，将正文进行反序列化
        SetUserNicknameReq req;
        SetUserNicknameRsp rsp;
        auto err_response = [&req, &rsp, &response](const std::string &errmsg) -> void {
            rsp.set_success(false);
            rsp.set_errmsg(errmsg);
            response.set_content(rsp.SerializeAsString(), "application/x-protbuf");
        };
        bool ret = req.ParseFromString(request.body);
        if(ret == false) {
            LOG_ERROR("设置用户昵称请求正文反序列化失败");
            return err_response("设置用户昵称请求正文反序列化失败");
        }
        //2. 客户端身份识别与鉴权
        std::string ssid = req.session_id();
        auto uid = _redis_session->uid(ssid);
        if(!uid) {
            LOG_ERROR("请求ID - {} 获取登录会话 {} 关联用户信息失败", req.request_id(), ssid);
            return err_response("获取登录会话关联用户信息失败");
        }
        req.set_user_id(*uid);
        //3. 将请求转发给用户子服务进行业务处理
        auto channel = _mm_channels->choose(_user_service_name);
        if(!channel) {
            LOG_ERROR("请求ID - {} 未找到可提供业务的用户子服务节点", req.request_id());
            return err_response("未找到可提供业务的用户子服务节点");
        }
        UserService_Stub stub(channel.get());
        brpc::Controller cntl;
        stub.SetUserNickname(&cntl, &req, &rsp, nullptr);
        if(cntl.Failed()) {
            LOG_ERROR("请求ID - {} 用户子服务调用失败: {}", req.request_id(), cntl.ErrorText());
            return err_response("用户子服务调用失败");
        }
        //4. 得到用户子服务的响应后，将响应进行序列化作为http响应正文
        response.set_content(rsp.SerializeAsString(), "application/x-protbuf");
    }
    void SetUserDescription(const httplib::Request &request, httplib::Response &response) {
        //1. 取出http请求正文，将正文进行反序列化
        SetUserDescriptionReq req;
        SetUserDescriptionRsp rsp;
        auto err_response = [&req, &rsp, &response](const std::string &errmsg) -> void {
            rsp.set_success(false);
            rsp.set_errmsg(errmsg);
            response.set_content(rsp.SerializeAsString(), "application/x-protbuf");
        };
        bool ret = req.ParseFromString(request.body);
        if(ret == false) {
            LOG_ERROR("设置用户签名请求正文反序列化失败");
            return err_response("设置用户签名请求正文反序列化失败");
        }
        //2. 客户端身份识别与鉴权
        std::string ssid = req.session_id();
        auto uid = _redis_session->uid(ssid);
        if(!uid) {
            LOG_ERROR("请求ID - {} 获取登录会话 {} 关联用户信息失败", req.request_id(), ssid);
            return err_response("获取登录会话关联用户信息失败");
        }
        req.set_user_id(*uid);
        //3. 将请求转发给用户子服务进行业务处理
        auto channel = _mm_channels->choose(_user_service_name);
        if(!channel) {
            LOG_ERROR("请求ID - {} 未找到可提供业务的用户子服务节点", req.request_id());
            return err_response("未找到可提供业务的用户子服务节点");
        }
        UserService_Stub stub(channel.get());
        brpc::Controller cntl;
        stub.SetUserDescription(&cntl, &req, &rsp, nullptr);
        if(cntl.Failed()) {
            LOG_ERROR("请求ID - {} 用户子服务调用失败: {}", req.request_id(), cntl.ErrorText());
            return err_response("用户子服务调用失败");
        }
        //4. 得到用户子服务的响应后，将响应进行序列化作为http响应正文
        response.set_content(rsp.SerializeAsString(), "application/x-protbuf");
    }
    void SetUserMailNumber(const httplib::Request &request, httplib::Response &response) {
        //1. 取出http请求正文，将正文进行反序列化
        SetUserMailNumberReq req;
        SetUserMailNumberRsp rsp;
        auto err_response = [&req, &rsp, &response](const std::string &errmsg) -> void {
            rsp.set_success(false);
            rsp.set_errmsg(errmsg);
            response.set_content(rsp.SerializeAsString(), "application/x-protbuf");
        };
        bool ret = req.ParseFromString(request.body);
        if(ret == false) {
            LOG_ERROR("设置用户邮箱请求正文反序列化失败");
            return err_response("设置用户邮箱请求正文反序列化失败");
        }
        //2. 客户端身份识别与鉴权
        std::string ssid = req.session_id();
        auto uid = _redis_session->uid(ssid);
        if(!uid) {
            LOG_ERROR("请求ID - {} 获取登录会话 {} 关联用户信息失败", req.request_id(), ssid);
            return err_response("获取登录会话关联用户信息失败");
        }
        req.set_user_id(*uid);
        //3. 将请求转发给用户子服务进行业务处理
        auto channel = _mm_channels->choose(_user_service_name);
        if(!channel) {
            LOG_ERROR("请求ID - {} 未找到可提供业务的用户子服务节点", req.request_id());
            return err_response("未找到可提供业务的用户子服务节点");
        }
        UserService_Stub stub(channel.get());
        brpc::Controller cntl;
        stub.SetUserMailNumber(&cntl, &req, &rsp, nullptr);
        if(cntl.Failed()) {
            LOG_ERROR("请求ID - {} 用户子服务调用失败: {}", req.request_id(), cntl.ErrorText());
            return err_response("用户子服务调用失败");
        }
        //4. 得到用户子服务的响应后，将响应进行序列化作为http响应正文
        response.set_content(rsp.SerializeAsString(), "application/x-protbuf");
    }
    void GetFriendList(const httplib::Request &request, httplib::Response &response) {
        //1. 取出http请求正文，将正文进行反序列化
        GetFriendListReq req;
        GetFriendListRsp rsp;
        auto err_response = [&req, &rsp, &response](const std::string &errmsg) -> void {
            rsp.set_success(false);
            rsp.set_errmsg(errmsg);
            response.set_content(rsp.SerializeAsString(), "application/x-protbuf");
        };
        bool ret = req.ParseFromString(request.body);
        if(ret == false) {
            LOG_ERROR("获取好友列表请求正文反序列化失败");
            return err_response("获取好友列表请求正文反序列化失败");
        }
        //2. 客户端身份识别与鉴权
        std::string ssid = req.session_id();
        auto uid = _redis_session->uid(ssid);
        if(!uid) {
            LOG_ERROR("请求ID - {} 获取登录会话 {} 关联用户信息失败", req.request_id(), ssid);
            return err_response("获取登录会话关联用户信息失败");
        }
        req.set_user_id(*uid);
        //3. 将请求转发给好友子服务进行业务处理
        auto channel = _mm_channels->choose(_friend_service_name);
        if(!channel) {
            LOG_ERROR("请求ID - {} 未找到可提供业务的好友子服务节点", req.request_id());
            return err_response("未找到可提供业务的好友子服务节点");
        }
        FriendService_Stub stub(channel.get());
        brpc::Controller cntl;
        stub.GetFriendList(&cntl, &req, &rsp, nullptr);
        if(cntl.Failed()) {
            LOG_ERROR("请求ID - {} 好友子服务调用失败: {}", req.request_id(), cntl.ErrorText());
            return err_response("好友子服务调用失败");
        }
        //4. 得到好友子服务的响应后，将响应进行序列化作为http响应正文
        response.set_content(rsp.SerializeAsString(), "application/x-protbuf");
    }

    std::shared_ptr<GetUserInfoRsp> _GetUserInfo(const std::string &rid, const std::string &uid) {
        //1. 取出http请求正文，将正文进行反序列化
        GetUserInfoReq req;
        auto rsp = std::make_shared<GetUserInfoRsp>();
        req.set_request_id(rid);
        req.set_user_id(uid);
        //3. 将请求转发给用户子服务进行业务处理
        auto channel = _mm_channels->choose(_user_service_name);
        if(!channel) {
            LOG_ERROR("请求ID - {} 未找到可提供业务的用户子服务节点", req.request_id());
            return std::shared_ptr<GetUserInfoRsp>();
        }
        UserService_Stub stub(channel.get());
        brpc::Controller cntl;
        stub.GetUserInfo(&cntl, &req, rsp.get(), nullptr);
        if(cntl.Failed()) {
            LOG_ERROR("请求ID - {} 用户子服务调用失败: {}", req.request_id(), cntl.ErrorText());
            return std::shared_ptr<GetUserInfoRsp>();
        }
        //4. 得到用户子服务的响应
        return rsp;
    }

    void FriendAdd(const httplib::Request &request, httplib::Response &response) {
        // 好友申请的业务处理中，好友子服务只是在数据库创建了申请事件
        // 网关需要做的事件：当好友子服务将业务处理完毕后，如果处理成功 -- 需要通知被申请方
        //1. 正文反序列化，提取关键要素：登录会话ID
        FriendAddReq req;
        FriendAddRsp rsp;
        auto err_response = [&req, &rsp, &response](const std::string &errmsg) -> void {
            rsp.set_success(false);
            rsp.set_errmsg(errmsg);
            response.set_content(rsp.SerializeAsString(), "application/x-protbuf");
        };
        bool ret = req.ParseFromString(request.body);
        if(ret == false) {
            LOG_ERROR("申请好友请求正文反序列化失败");
            return err_response("申请好友请求正文反序列化失败");
        }
        //2. 客户端身份识别和鉴权
        std::string ssid = req.session_id();
        auto uid = _redis_session->uid(ssid);
        if(!uid) {
            LOG_ERROR("请求ID - {} 获取登录会话 {} 关联用户信息失败", req.request_id(), ssid);
            return err_response("获取登录会话关联用户信息失败");
        }
        req.set_user_id(*uid);
        //3. 将请求转发给好友子服务进行业务处理
        auto channel = _mm_channels->choose(_friend_service_name);
        if(!channel) {
            LOG_ERROR("请求ID - {} 未找到可提供业务的好友子服务节点", req.request_id());
            return err_response("未找到可提供业务的好友子服务节点");
        }
        FriendService_Stub stub(channel.get());
        brpc::Controller cntl;
        stub.FriendAdd(&cntl, &req, &rsp, nullptr);
        if(cntl.Failed()) {
            LOG_ERROR("请求ID - {} 好友子服务调用失败: {}", req.request_id(), cntl.ErrorText());
            return err_response("好友子服务调用失败");
        }
        //4. 业务处理成功 → 向被申请人推送 FRIEND_ADD_APPLY_NOTIFY（通过 Push 服务，多实例可达）
        if(rsp.success()) {
            auto user_rsp = _GetUserInfo(req.request_id(), *uid);
            if(!user_rsp) {
                LOG_ERROR("请求ID - {} 获取当前客户端用户信息失败", req.request_id());
                return err_response("获取当前客户端用户信息失败");
            }
            NotifyMessage notify;
            notify.set_notify_type(NotifyType::FRIEND_ADD_APPLY_NOTIFY);
            notify.mutable_friend_add_apply()->mutable_user_info()->CopyFrom(user_rsp->user_info());
            _pushNotify(req.respondent_id(), notify, req.request_id());
        }
        //5. 向客户端进行响应
        response.set_content(rsp.SerializeAsString(), "application/x-protbuf");
    }
    void FriendAddProcess(const httplib::Request &request, httplib::Response &response) {
        //好友申请处理
        //1. 反序列化请求正文，提取要素：登录会话ID，处理结果，申请人
        FriendAddProcessReq req;
        FriendAddProcessRsp rsp;
        auto err_response = [&req, &rsp, &response](const std::string &errmsg) -> void {
            rsp.set_success(false);
            rsp.set_errmsg(errmsg);
            response.set_content(rsp.SerializeAsString(), "application/x-protbuf");
        };
        bool ret = req.ParseFromString(request.body);
        if(ret == false) {
            LOG_ERROR("好友申请处理请求正文反序列化失败");
            return err_response("好友申请处理请求正文反序列化失败");
        }
        //2. 客户端身份识别鉴权，并获取处理人用户ID
        std::string ssid = req.session_id();
        auto uid = _redis_session->uid(ssid);
        if(!uid) {
            LOG_ERROR("请求ID - {} 获取登录会话 {} 关联用户信息失败", req.request_id(), ssid);
            return err_response("获取登录会话关联用户信息失败");
        }
        req.set_user_id(*uid);
        //3. 将请求转发给好友子服务进行业务处理
        auto channel = _mm_channels->choose(_friend_service_name);
        if(!channel) {
            LOG_ERROR("请求ID - {} 未找到可提供业务的好友子服务节点", req.request_id());
            return err_response("未找到可提供业务的好友子服务节点");
        }
        FriendService_Stub stub(channel.get());
        brpc::Controller cntl;
        stub.FriendAddProcess(&cntl, &req, &rsp, nullptr);
        if(cntl.Failed()) {
            LOG_ERROR("请求ID - {} 好友子服务调用失败: {}", req.request_id(), cntl.ErrorText());
            return err_response("好友子服务调用失败");
        }


        if(rsp.success()) {
            auto process_user_rsp = _GetUserInfo(req.request_id(), *uid);
            if(!process_user_rsp) {
                LOG_ERROR("请求ID - {} 获取用户信息失败", req.request_id());
                return err_response("获取用户信息失败");
            }
            auto apply_user_rsp = _GetUserInfo(req.request_id(), req.apply_user_id());
            if(!apply_user_rsp) {
                LOG_ERROR("请求ID - {} 获取用户信息失败", req.request_id());
                return err_response("获取用户信息失败");
            }
            //4. 将处理结果给申请人推送（无条件，Push 服务负责离线兜底）
            {
                NotifyMessage notify;
                notify.set_notify_type(NotifyType::FRIEND_ADD_PROCESS_NOTIFY);
                auto process_result = notify.mutable_friend_process_result();
                process_result->mutable_user_info()->CopyFrom(process_user_rsp->user_info());
                process_result->set_agree(req.agree());
                _pushNotify(req.apply_user_id(), notify, req.request_id());
            }
            //5. 若同意 → 双向推送会话创建通知
            if(req.agree()) {
                // 5.1 给申请人：会话信息为处理人信息
                NotifyMessage notify;
                notify.set_notify_type(NotifyType::CHAT_SESSION_CREATE_NOTIFY);
                auto chat_session = notify.mutable_new_chat_session_info();
                chat_session->mutable_chat_session_info()->set_single_chat_friend_id(*uid);
                chat_session->mutable_chat_session_info()->set_chat_session_id(rsp.new_session_id());
                chat_session->mutable_chat_session_info()->set_chat_session_name(process_user_rsp->user_info().nickname());
                chat_session->mutable_chat_session_info()->set_avatar(process_user_rsp->user_info().avatar());
                _pushNotify(req.apply_user_id(), notify, req.request_id());

                // 5.2 给处理人：会话信息为申请人信息
                NotifyMessage notify2;
                notify2.set_notify_type(NotifyType::CHAT_SESSION_CREATE_NOTIFY);
                auto chat_session2 = notify2.mutable_new_chat_session_info();
                chat_session2->mutable_chat_session_info()->set_single_chat_friend_id(req.apply_user_id());
                chat_session2->mutable_chat_session_info()->set_chat_session_id(rsp.new_session_id());
                chat_session2->mutable_chat_session_info()->set_chat_session_name(apply_user_rsp->user_info().nickname());
                chat_session2->mutable_chat_session_info()->set_avatar(apply_user_rsp->user_info().avatar());
                _pushNotify(*uid, notify2, req.request_id());
            }
        }
        //6. 对客户端进行响应
        response.set_content(rsp.SerializeAsString(), "application/x-protbuf");
    }
    void FriendRemove(const httplib::Request &request, httplib::Response &response) {
        //1. 正文反序列化，提取关键要素：登录会话ID
        FriendRemoveReq req;
        FriendRemoveRsp rsp;
        auto err_response = [&req, &rsp, &response](const std::string &errmsg) -> void {
            rsp.set_success(false);
            rsp.set_errmsg(errmsg);
            response.set_content(rsp.SerializeAsString(), "application/x-protbuf");
        };
        bool ret = req.ParseFromString(request.body);
        if(ret == false) {
            LOG_ERROR("删除好友请求正文反序列化失败");
            return err_response("删除好友请求正文反序列化失败");
        }
        //2. 客户端身份识别和鉴权
        std::string ssid = req.session_id();
        auto uid = _redis_session->uid(ssid);
        if(!uid) {
            LOG_ERROR("请求ID - {} 获取登录会话 {} 关联用户信息失败", req.request_id(), ssid);
            return err_response("获取登录会话关联用户信息失败");
        }
        req.set_user_id(*uid);
        //3. 将请求转发给好友子服务进行业务处理
        auto channel = _mm_channels->choose(_friend_service_name);
        if(!channel) {
            LOG_ERROR("请求ID - {} 未找到可提供业务的好友子服务节点", req.request_id());
            return err_response("未找到可提供业务的好友子服务节点");
        }
        FriendService_Stub stub(channel.get());
        brpc::Controller cntl;
        stub.FriendRemove(&cntl, &req, &rsp, nullptr);
        if(cntl.Failed()) {
            LOG_ERROR("请求ID - {} 好友子服务调用失败: {}", req.request_id(), cntl.ErrorText());
            return err_response("好友子服务调用失败");
        }
        //4. 业务成功 → 向被删除人推送 FRIEND_REMOVE_NOTIFY（多实例可达）
        if(rsp.success()) {
            NotifyMessage notify;
            notify.set_notify_type(NotifyType::FRIEND_REMOVE_NOTIFY);
            notify.mutable_friend_remove()->set_user_id(*uid);
            _pushNotify(req.peer_id(), notify, req.request_id());
        }
        //5. 向客户端进行响应
        response.set_content(rsp.SerializeAsString(), "application/x-protbuf");
    }
    void FriendSearch(const httplib::Request &request, httplib::Response &response) {
        //1. 正文反序列化，提取关键要素：登录会话ID
        FriendSearchReq req;
        FriendSearchRsp rsp;
        auto err_response = [&req, &rsp, &response](const std::string &errmsg) -> void {
            rsp.set_success(false);
            rsp.set_errmsg(errmsg);
            response.set_content(rsp.SerializeAsString(), "application/x-protbuf");
        };
        bool ret = req.ParseFromString(request.body);
        if(ret == false) {
            LOG_ERROR("用户搜索请求正文反序列化失败");
            return err_response("用户搜索请求正文反序列化失败");
        }
        //2. 客户端身份识别和鉴权
        std::string ssid = req.session_id();
        auto uid = _redis_session->uid(ssid);
        if(!uid) {
            LOG_ERROR("请求ID - {} 获取登录会话 {} 关联用户信息失败", req.request_id(), ssid);
            return err_response("获取登录会话关联用户信息失败");
        }
        req.set_user_id(*uid);
        //3. 将请求转发给好友子服务进行业务处理
        auto channel = _mm_channels->choose(_friend_service_name);
        if(!channel) {
            LOG_ERROR("请求ID - {} 未找到可提供业务的好友子服务节点", req.request_id());
            return err_response("未找到可提供业务的好友子服务节点");
        }
        FriendService_Stub stub(channel.get());
        brpc::Controller cntl;
        stub.FriendSearch(&cntl, &req, &rsp, nullptr);
        if(cntl.Failed()) {
            LOG_ERROR("请求ID - {} 好友子服务调用失败: {}", req.request_id(), cntl.ErrorText());
            return err_response("好友子服务调用失败");
        }
        //5. 向客户端进行响应
        response.set_content(rsp.SerializeAsString(), "application/x-protbuf");
    }
    void GetPendingFriendEventList(const httplib::Request &request, httplib::Response &response) {
        //1. 正文反序列化，提取关键要素：登录会话ID
        GetPendingFriendEventListReq req;
        GetPendingFriendEventListRsp rsp;
        auto err_response = [&req, &rsp, &response](const std::string &errmsg) -> void {
            rsp.set_success(false);
            rsp.set_errmsg(errmsg);
            response.set_content(rsp.SerializeAsString(), "application/x-protbuf");
        };
        bool ret = req.ParseFromString(request.body);
        if(ret == false) {
            LOG_ERROR("获取待处理好友申请请求正文反序列化失败");
            return err_response("获取待处理好友申请请求正文反序列化失败");
        }
        //2. 客户端身份识别和鉴权
        std::string ssid = req.session_id();
        auto uid = _redis_session->uid(ssid);
        if(!uid) {
            LOG_ERROR("请求ID - {} 获取登录会话 {} 关联用户信息失败", req.request_id(), ssid);
            return err_response("获取登录会话关联用户信息失败");
        }
        req.set_user_id(*uid);
        //3. 将请求转发给好友子服务进行业务处理
        auto channel = _mm_channels->choose(_friend_service_name);
        if(!channel) {
            LOG_ERROR("请求ID - {} 未找到可提供业务的好友子服务节点", req.request_id());
            return err_response("未找到可提供业务的好友子服务节点");
        }
        FriendService_Stub stub(channel.get());
        brpc::Controller cntl;
        stub.GetPendingFriendEventList(&cntl, &req, &rsp, nullptr);
        if(cntl.Failed()) {
            LOG_ERROR("请求ID - {} 好友子服务调用失败: {}", req.request_id(), cntl.ErrorText());
            return err_response("好友子服务调用失败");
        }
        //5. 向客户端进行响应
        response.set_content(rsp.SerializeAsString(), "application/x-protbuf");
    }
    void GetChatSessionList(const httplib::Request &request, httplib::Response &response) {
        //1. 正文反序列化，提取关键要素：登录会话ID
        GetChatSessionListReq req;
        GetChatSessionListRsp rsp;
        auto err_response = [&req, &rsp, &response](const std::string &errmsg) -> void {
            rsp.set_success(false);
            rsp.set_errmsg(errmsg);
            response.set_content(rsp.SerializeAsString(), "application/x-protbuf");
        };
        bool ret = req.ParseFromString(request.body);
        if(ret == false) {
            LOG_ERROR("获取聊天会话列表请求正文反序列化失败");
            return err_response("获取聊天会话列表请求正文反序列化失败");
        }
        //2. 客户端身份识别和鉴权
        std::string ssid = req.session_id();
        auto uid = _redis_session->uid(ssid);
        if(!uid) {
            LOG_ERROR("请求ID - {} 获取登录会话 {} 关联用户信息失败", req.request_id(), ssid);
            return err_response("获取登录会话关联用户信息失败");
        }
        req.set_user_id(*uid);
        //3. 将请求转发给好友子服务进行业务处理
        auto channel = _mm_channels->choose(_friend_service_name);
        if(!channel) {
            LOG_ERROR("请求ID - {} 未找到可提供业务的好友子服务节点", req.request_id());
            return err_response("未找到可提供业务的好友子服务节点");
        }
        ChatSessionService_Stub stub(channel.get());
        brpc::Controller cntl;
        stub.GetChatSessionList(&cntl, &req, &rsp, nullptr);
        if(cntl.Failed()) {
            LOG_ERROR("请求ID - {} 好友子服务调用失败: {}", req.request_id(), cntl.ErrorText());
            return err_response("好友子服务调用失败");
        }
        //5. 向客户端进行响应
        response.set_content(rsp.SerializeAsString(), "application/x-protbuf");
    }
    void GetChatSessionMember(const httplib::Request &request, httplib::Response &response) {
        //1. 正文反序列化，提取关键要素：登录会话ID
        GetChatSessionMemberReq req;
        GetChatSessionMemberRsp rsp;
        auto err_response = [&req, &rsp, &response](const std::string &errmsg) -> void {
            rsp.set_success(false);
            rsp.set_errmsg(errmsg);
            response.set_content(rsp.SerializeAsString(), "application/x-protbuf");
        };
        bool ret = req.ParseFromString(request.body);
        if(ret == false) {
            LOG_ERROR("获取聊天会话成员列表请求正文反序列化失败");
            return err_response("获取聊天会话成员列表请求正文反序列化失败");
        }
        //2. 客户端身份识别和鉴权
        std::string ssid = req.session_id();
        auto uid = _redis_session->uid(ssid);
        if(!uid) {
            LOG_ERROR("请求ID - {} 获取登录会话 {} 关联用户信息失败", req.request_id(), ssid);
            return err_response("获取登录会话关联用户信息失败");
        }
        req.set_user_id(*uid);
        //3. 将请求转发给好友子服务进行业务处理
        auto channel = _mm_channels->choose(_friend_service_name);
        if(!channel) {
            LOG_ERROR("请求ID - {} 未找到可提供业务的好友子服务节点", req.request_id());
            return err_response("未找到可提供业务的好友子服务节点");
        }
        ChatSessionService_Stub stub(channel.get());
        brpc::Controller cntl;
        stub.GetChatSessionMember(&cntl, &req, &rsp, nullptr);
        if(cntl.Failed()) {
            LOG_ERROR("请求ID - {} 好友子服务调用失败: {}", req.request_id(), cntl.ErrorText());
            return err_response("好友子服务调用失败");
        }
        //5. 向客户端进行响应
        response.set_content(rsp.SerializeAsString(), "application/x-protbuf");
    }
    void ChatSessionCreate(const httplib::Request &request, httplib::Response &response) {
        //1. 正文反序列化，提取关键要素：登录会话ID
        ChatSessionCreateReq req;
        ChatSessionCreateRsp rsp;
        auto err_response = [&req, &rsp, &response](const std::string &errmsg) -> void {
            rsp.set_success(false);
            rsp.set_errmsg(errmsg);
            response.set_content(rsp.SerializeAsString(), "application/x-protbuf");
        };
        bool ret = req.ParseFromString(request.body);
        if(ret == false) {
            LOG_ERROR("创建聊天会话请求正文反序列化失败");
            return err_response("创建聊天会话请求正文反序列化失败");
        }
        //2. 客户端身份识别和鉴权
        std::string ssid = req.session_id();
        auto uid = _redis_session->uid(ssid);
        if(!uid) {
            LOG_ERROR("请求ID - {} 获取登录会话 {} 关联用户信息失败", req.request_id(), ssid);
            return err_response("获取登录会话关联用户信息失败");
        }
        req.set_user_id(*uid);
        //3. 将请求转发给好友子服务进行业务处理
        auto channel = _mm_channels->choose(_friend_service_name);
        if(!channel) {
            LOG_ERROR("请求ID - {} 未找到可提供业务的好友子服务节点", req.request_id());
            return err_response("未找到可提供业务的好友子服务节点");
        }
        ChatSessionService_Stub stub(channel.get());
        brpc::Controller cntl;
        stub.ChatSessionCreate(&cntl, &req, &rsp, nullptr);
        if(cntl.Failed()) {
            LOG_ERROR("请求ID - {} 好友子服务调用失败: {}", req.request_id(), cntl.ErrorText());
            return err_response("好友子服务调用失败");
        }
        //4. 业务成功 → 向所有成员推送 CHAT_SESSION_CREATE_NOTIFY（多实例可达）
        if(rsp.success()) {
            for(int i = 0; i < req.member_id_list_size(); ++i) {
                NotifyMessage notify;
                notify.set_notify_type(NotifyType::CHAT_SESSION_CREATE_NOTIFY);
                auto chat_session = notify.mutable_new_chat_session_info();
                chat_session->mutable_chat_session_info()->CopyFrom(rsp.chat_session_info());
                _pushNotify(req.member_id_list(i), notify, req.request_id());
            }
        }
        //5. 向客户端进行响应
        rsp.clear_chat_session_info();
        response.set_content(rsp.SerializeAsString(), "application/x-protbuf");
    }
    void GetHistoryMsg(const httplib::Request &request, httplib::Response &response) {
        //1. 正文反序列化，提取关键要素：登录会话ID
        GetHistoryMsgReq req;
        GetHistoryMsgRsp rsp;
        auto err_response = [&req, &rsp, &response](const std::string &errmsg) -> void {
            rsp.set_success(false);
            rsp.set_errmsg(errmsg);
            response.set_content(rsp.SerializeAsString(), "application/x-protbuf");
        };
        bool ret = req.ParseFromString(request.body);
        if(ret == false) {
            LOG_ERROR("获取历史消息请求正文反序列化失败");
            return err_response("获取历史消息请求正文反序列化失败");
        }
        //2. 客户端身份识别和鉴权
        std::string ssid = req.session_id();
        auto uid = _redis_session->uid(ssid);
        if(!uid) {
            LOG_ERROR("请求ID - {} 获取登录会话 {} 关联用户信息失败", req.request_id(), ssid);
            return err_response("获取登录会话关联用户信息失败");
        }
        req.set_user_id(*uid);
        //3. 将请求转发给消息存储子服务进行业务处理
        auto channel = _mm_channels->choose(_message_service_name);
        if(!channel) {
            LOG_ERROR("请求ID - {} 未找到可提供业务的消息存储子服务节点", req.request_id());
            return err_response("未找到可提供业务的消息存储子服务节点");
        }
        MsgStorageService_Stub stub(channel.get());
        brpc::Controller cntl;
        stub.GetHistoryMsg(&cntl, &req, &rsp, nullptr);
        if(cntl.Failed()) {
            LOG_ERROR("请求ID - {} 消息存储子服务调用失败: {}", req.request_id(), cntl.ErrorText());
            return err_response("消息存储子服务调用失败");
        }
        //5. 向客户端进行响应
        response.set_content(rsp.SerializeAsString(), "application/x-protbuf");
    }
    void GetRecentMsg(const httplib::Request &request, httplib::Response &response) {
        //1. 正文反序列化，提取关键要素：登录会话ID
        GetRecentMsgReq req;
        GetRecentMsgRsp rsp;
        auto err_response = [&req, &rsp, &response](const std::string &errmsg) -> void {
            rsp.set_success(false);
            rsp.set_errmsg(errmsg);
            response.set_content(rsp.SerializeAsString(), "application/x-protbuf");
        };
        bool ret = req.ParseFromString(request.body);
        if(ret == false) {
            LOG_ERROR("获取最近消息请求正文反序列化失败");
            return err_response("获取最近消息请求正文反序列化失败");
        }
        //2. 客户端身份识别和鉴权
        std::string ssid = req.session_id();
        auto uid = _redis_session->uid(ssid);
        if(!uid) {
            LOG_ERROR("请求ID - {} 获取登录会话 {} 关联用户信息失败", req.request_id(), ssid);
            return err_response("获取登录会话关联用户信息失败");
        }
        req.set_user_id(*uid);
        //3. 将请求转发给消息存储子服务进行业务处理
        auto channel = _mm_channels->choose(_message_service_name);
        if(!channel) {
            LOG_ERROR("请求ID - {} 未找到可提供业务的消息存储子服务节点", req.request_id());
            return err_response("未找到可提供业务的消息存储子服务节点");
        }
        MsgStorageService_Stub stub(channel.get());
        brpc::Controller cntl;
        stub.GetRecentMsg(&cntl, &req, &rsp, nullptr);
        if(cntl.Failed()) {
            LOG_ERROR("请求ID - {} 消息存储子服务调用失败: {}", req.request_id(), cntl.ErrorText());
            return err_response("消息存储子服务调用失败");
        }
        //5. 向客户端进行响应
        response.set_content(rsp.SerializeAsString(), "application/x-protbuf");
    }
    void MsgSearch(const httplib::Request &request, httplib::Response &response) {
        //1. 正文反序列化，提取关键要素：登录会话ID
        MsgSearchReq req;
        MsgSearchRsp rsp;
        auto err_response = [&req, &rsp, &response](const std::string &errmsg) -> void {
            rsp.set_success(false);
            rsp.set_errmsg(errmsg);
            response.set_content(rsp.SerializeAsString(), "application/x-protbuf");
        };
        bool ret = req.ParseFromString(request.body);
        if(ret == false) {
            LOG_ERROR("消息搜索请求正文反序列化失败");
            return err_response("消息搜索请求正文反序列化失败");
        }
        //2. 客户端身份识别和鉴权
        std::string ssid = req.session_id();
        auto uid = _redis_session->uid(ssid);
        if(!uid) {
            LOG_ERROR("请求ID - {} 获取登录会话 {} 关联用户信息失败", req.request_id(), ssid);
            return err_response("获取登录会话关联用户信息失败");
        }
        req.set_user_id(*uid);
        //3. 将请求转发给消息存储子服务进行业务处理
        auto channel = _mm_channels->choose(_message_service_name);
        if(!channel) {
            LOG_ERROR("请求ID - {} 未找到可提供业务的消息存储子服务节点", req.request_id());
            return err_response("未找到可提供业务的消息存储子服务节点");
        }
        MsgStorageService_Stub stub(channel.get());
        brpc::Controller cntl;
        stub.MsgSearch(&cntl, &req, &rsp, nullptr);
        if(cntl.Failed()) {
            LOG_ERROR("请求ID - {} 消息存储子服务调用失败: {}", req.request_id(), cntl.ErrorText());
            return err_response("消息存储子服务调用失败");
        }
        //5. 向客户端进行响应
        response.set_content(rsp.SerializeAsString(), "application/x-protbuf");
    }
    void GetSingleFile(const httplib::Request &request, httplib::Response &response) {
        //1. 正文反序列化，提取关键要素：登录会话ID
        GetSingleFileReq req;
        GetSingleFileRsp rsp;
        auto err_response = [&req, &rsp, &response](const std::string &errmsg) -> void {
            rsp.set_success(false);
            rsp.set_errmsg(errmsg);
            response.set_content(rsp.SerializeAsString(), "application/x-protbuf");
        };
        bool ret = req.ParseFromString(request.body);
        if(ret == false) {
            LOG_ERROR("获取单文件请求正文反序列化失败");
            return err_response("获取单文件请求正文反序列化失败");
        }
        //2. 客户端身份识别和鉴权
        std::string ssid = req.session_id();
        auto uid = _redis_session->uid(ssid);
        if(!uid) {
            LOG_ERROR("请求ID - {} 获取登录会话 {} 关联用户信息失败", req.request_id(), ssid);
            return err_response("获取登录会话关联用户信息失败");
        }
        req.set_user_id(*uid);
        //3. 将请求转发给文件存储子服务进行业务处理
        auto channel = _mm_channels->choose(_file_service_name);
        if(!channel) {
            LOG_ERROR("请求ID - {} 未找到可提供业务的文件存储子服务节点", req.request_id());
            return err_response("未找到可提供业务的文件存储子服务节点");
        }
        FileService_Stub stub(channel.get());
        brpc::Controller cntl;
        stub.GetSingleFile(&cntl, &req, &rsp, nullptr);
        if(cntl.Failed()) {
            LOG_ERROR("请求ID - {} 文件存储子服务调用失败: {}", req.request_id(), cntl.ErrorText());
            return err_response("文件存储子服务调用失败");
        }
        //5. 向客户端进行响应
        response.set_content(rsp.SerializeAsString(), "application/x-protbuf");
    }
    void GetMultiFile(const httplib::Request &request, httplib::Response &response) {
        //1. 正文反序列化，提取关键要素：登录会话ID
        GetMultiFileReq req;
        GetMultiFileRsp rsp;
        auto err_response = [&req, &rsp, &response](const std::string &errmsg) -> void {
            rsp.set_success(false);
            rsp.set_errmsg(errmsg);
            response.set_content(rsp.SerializeAsString(), "application/x-protbuf");
        };
        bool ret = req.ParseFromString(request.body);
        if(ret == false) {
            LOG_ERROR("获取多文件请求正文反序列化失败");
            return err_response("获取多文件请求正文反序列化失败");
        }
        //2. 客户端身份识别和鉴权
        std::string ssid = req.session_id();
        auto uid = _redis_session->uid(ssid);
        if(!uid) {
            LOG_ERROR("请求ID - {} 获取登录会话 {} 关联用户信息失败", req.request_id(), ssid);
            return err_response("获取登录会话关联用户信息失败");
        }
        req.set_user_id(*uid);
        //3. 将请求转发给文件存储子服务进行业务处理
        auto channel = _mm_channels->choose(_file_service_name);
        if(!channel) {
            LOG_ERROR("请求ID - {} 未找到可提供业务的文件存储子服务节点", req.request_id());
            return err_response("未找到可提供业务的文件存储子服务节点");
        }
        FileService_Stub stub(channel.get());
        brpc::Controller cntl;
        stub.GetMultiFile(&cntl, &req, &rsp, nullptr);
        if(cntl.Failed()) {
            LOG_ERROR("请求ID - {} 文件存储子服务调用失败: {}", req.request_id(), cntl.ErrorText());
            return err_response("文件存储子服务调用失败");
        }
        //5. 向客户端进行响应
        response.set_content(rsp.SerializeAsString(), "application/x-protbuf");
    }
    void PutSingleFile(const httplib::Request &request, httplib::Response &response) {
        //1. 正文反序列化，提取关键要素：登录会话ID
        PutSingleFileReq req;
        PutSingleFileRsp rsp;
        auto err_response = [&req, &rsp, &response](const std::string &errmsg) -> void {
            rsp.set_success(false);
            rsp.set_errmsg(errmsg);
            response.set_content(rsp.SerializeAsString(), "application/x-protbuf");
        };
        bool ret = req.ParseFromString(request.body);
        if(ret == false) {
            LOG_ERROR("上传单文件请求正文反序列化失败");
            return err_response("上传单文件请求正文反序列化失败");
        }
        //2. 客户端身份识别和鉴权
        std::string ssid = req.session_id();
        auto uid = _redis_session->uid(ssid);
        if(!uid) {
            LOG_ERROR("请求ID - {} 获取登录会话 {} 关联用户信息失败", req.request_id(), ssid);
            return err_response("获取登录会话关联用户信息失败");
        }
        req.set_user_id(*uid);
        //3. 将请求转发给文件存储子服务进行业务处理
        auto channel = _mm_channels->choose(_file_service_name);
        if(!channel) {
            LOG_ERROR("请求ID - {} 未找到可提供业务的文件存储子服务节点", req.request_id());
            return err_response("未找到可提供业务的文件存储子服务节点");
        }
        FileService_Stub stub(channel.get());
        brpc::Controller cntl;
        stub.PutSingleFile(&cntl, &req, &rsp, nullptr);
        if(cntl.Failed()) {
            LOG_ERROR("请求ID - {} 文件存储子服务调用失败: {}", req.request_id(), cntl.ErrorText());
            return err_response("文件存储子服务调用失败");
        }
        //5. 向客户端进行响应
        response.set_content(rsp.SerializeAsString(), "application/x-protbuf");
    }
    void PutMultiFile(const httplib::Request &request, httplib::Response &response) {
        //1. 正文反序列化，提取关键要素：登录会话ID
        PutMultiFileReq req;
        PutMultiFileRsp rsp;
        auto err_response = [&req, &rsp, &response](const std::string &errmsg) -> void {
            rsp.set_success(false);
            rsp.set_errmsg(errmsg);
            response.set_content(rsp.SerializeAsString(), "application/x-protbuf");
        };
        bool ret = req.ParseFromString(request.body);
        if(ret == false) {
            LOG_ERROR("上传多文件请求正文反序列化失败");
            return err_response("上传多文件请求正文反序列化失败");
        }
        //2. 客户端身份识别和鉴权
        std::string ssid = req.session_id();
        auto uid = _redis_session->uid(ssid);
        if(!uid) {
            LOG_ERROR("请求ID - {} 获取登录会话 {} 关联用户信息失败", req.request_id(), ssid);
            return err_response("获取登录会话关联用户信息失败");
        }
        req.set_user_id(*uid);
        //3. 将请求转发给文件存储子服务进行业务处理
        auto channel = _mm_channels->choose(_file_service_name);
        if(!channel) {
            LOG_ERROR("请求ID - {} 未找到可提供业务的文件存储子服务节点", req.request_id());
            return err_response("未找到可提供业务的文件存储子服务节点");
        }
        FileService_Stub stub(channel.get());
        brpc::Controller cntl;
        stub.PutMultiFile(&cntl, &req, &rsp, nullptr);
        if(cntl.Failed()) {
            LOG_ERROR("请求ID - {} 文件存储子服务调用失败: {}", req.request_id(), cntl.ErrorText());
            return err_response("文件存储子服务调用失败");
        }
        //5. 向客户端进行响应
        response.set_content(rsp.SerializeAsString(), "application/x-protbuf");
    }
    void SpeechRecognition(const httplib::Request &request, httplib::Response &response) {
        //1. 正文反序列化，提取关键要素：登录会话ID
        SpeechRecognitionReq req;
        SpeechRecognitionRsp rsp;
        auto err_response = [&req, &rsp, &response](const std::string &errmsg) -> void {
            rsp.set_success(false);
            rsp.set_errmsg(errmsg);
            response.set_content(rsp.SerializeAsString(), "application/x-protbuf");
        };
        bool ret = req.ParseFromString(request.body);
        if(ret == false) {
            LOG_ERROR("语音识别请求正文反序列化失败");
            return err_response("语音识别请求正文反序列化失败");
        }
        //2. 客户端身份识别和鉴权
        std::string ssid = req.session_id();
        auto uid = _redis_session->uid(ssid);
        if(!uid) {
            LOG_ERROR("请求ID - {} 获取登录会话 {} 关联用户信息失败", req.request_id(), ssid);
            return err_response("获取登录会话关联用户信息失败");
        }
        req.set_user_id(*uid);
        //3. 将请求转发给语音识别子服务进行业务处理
        auto channel = _mm_channels->choose(_speech_service_name);
        if(!channel) {
            LOG_ERROR("请求ID - {} 未找到可提供业务的语音识别子服务节点", req.request_id());
            return err_response("未找到可提供业务的语音识别子服务节点");
        }
        SpeechService_Stub stub(channel.get());
        brpc::Controller cntl;
        stub.SpeechRecognition(&cntl, &req, &rsp, nullptr);
        if(cntl.Failed()) {
            LOG_ERROR("请求ID - {} 语音识别子服务调用失败: {}", req.request_id(), cntl.ErrorText());
            return err_response("语音识别子服务调用失败");
        }
        //5. 向客户端进行响应
        response.set_content(rsp.SerializeAsString(), "application/x-protbuf");
    }
    void NewMessage(const httplib::Request &request, httplib::Response &response) {
        //1. 正文反序列化，提取关键要素：登录会话ID
        NewMessageReq req;
        NewMessageRsp rsp;  //给客户端的响应
        GetTransmitTargetRsp target_rsp; //给请求子服务的响应
        auto err_response = [&req, &rsp, &response](const std::string &errmsg) -> void {
            rsp.set_success(false);
            rsp.set_errmsg(errmsg);
            response.set_content(rsp.SerializeAsString(), "application/x-protbuf");
        };
        bool ret = req.ParseFromString(request.body);
        if(ret == false) {
            LOG_ERROR("新消息请求正文反序列化失败");
            return err_response("新消息请求正文反序列化失败");
        }
        //2. 客户端身份识别和鉴权
        std::string ssid = req.session_id();
        auto uid = _redis_session->uid(ssid);
        if(!uid) {
            LOG_ERROR("请求ID - {} 获取登录会话 {} 关联用户信息失败", req.request_id(), ssid);
            return err_response("获取登录会话关联用户信息失败");
        }
        req.set_user_id(*uid);
        //3. 将请求转发给消息转发子服务进行业务处理
        auto channel = _mm_channels->choose(_transmite_service_name);
        if(!channel) {
            LOG_ERROR("请求ID - {} 未找到可提供业务的消息转发子服务节点", req.request_id());
            return err_response("未找到可提供业务的消息转发子服务节点");
        }
        MsgTransmitService_Stub stub(channel.get());
        brpc::Controller cntl;
        stub.GetTransmitTarget(&cntl, &req, &target_rsp, nullptr);
        if(cntl.Failed()) {
            LOG_ERROR("请求ID - {} 消息转发子服务调用失败: {}", req.request_id(), cntl.ErrorText());
            return err_response("消息转发子服务调用失败");
        }
        //4. 推送由 Push 服务通过 msg_push_queue 异步下发，Gateway 不再本机推送（无状态化）
        //   message 服务在 onDBMessage 写完 timeline 后会向 push_queue 投递一份 InternalMessage
        //   Push 服务消费后查 Redis 路由 → 跨实例 brpc 转发 → WebSocket 下发 + 待重传
        //5. 向客户端响应（带 message_id / seq_id 让客户端做事实校验）
        rsp.set_request_id(req.request_id());
        rsp.set_success(target_rsp.success());
        rsp.set_errmsg(target_rsp.errmsg());
        if(target_rsp.success()) {
            rsp.set_message_id(target_rsp.message_id());
            rsp.set_seq_id(target_rsp.seq_id());
        }
        response.set_content(rsp.SerializeAsString(), "application/x-protbuf");
    }
    //========================================================//
    //===================== V2.0 =============================//
    //========================================================//
    void GetChatSessionDetail(const httplib::Request &request, httplib::Response &response) {
        //1. 正文反序列化，提取关键要素：登录会话ID
        GetChatSessionDetailReq req;
        GetChatSessionDetailRsp rsp;  //给客户端的响应
        auto err_response = [&req, &rsp, &response](const std::string &errmsg) -> void {
            rsp.set_success(false);
            rsp.set_errmsg(errmsg);
            response.set_content(rsp.SerializeAsString(), "application/x-protbuf");
        };
        bool ret = req.ParseFromString(request.body);
        if(ret == false) {
            LOG_ERROR("获取会话详细信息请求正文反序列化失败");
            return err_response("获取会话详细信息请求正文反序列化失败");
        }
        //2. 客户端身份识别和鉴权
        std::string ssid = req.session_id();
        auto uid = _redis_session->uid(ssid);
        if(!uid) {
            LOG_ERROR("请求ID - {} 获取登录会话 {} 关联用户信息失败", req.request_id(), ssid);
            return err_response("获取登录会话关联用户信息失败");
        }
        req.set_user_id(*uid);
        //3. 将请求转发给消息转发子服务进行业务处理
        auto channel = _mm_channels->choose(_chatsession_service_name);
        if(!channel) {
            LOG_ERROR("请求ID - {} 未找到可提供业务的会话管理子服务节点", req.request_id());
            return err_response("未找到可提供业务的会话管理子服务节点");
        }
        ChatSessionService_Stub stub(channel.get());
        brpc::Controller cntl;
        stub.GetChatSessionDetail(&cntl, &req, &rsp, nullptr);
        if(cntl.Failed()) {
            LOG_ERROR("请求ID - {} 会话管理子服务调用失败: {}", req.request_id(), cntl.ErrorText());
            return err_response("会话管理子服务调用失败");
        }
        //5. 向客户端进行响应
        response.set_content(rsp.SerializeAsString(), "application/x-protbuf");
    }
    void SetChatSessionName(const httplib::Request &request, httplib::Response &response) {
        //1. 正文反序列化，提取关键要素：登录会话ID
        SetChatSessionNameReq req;
        SetChatSessionNameRsp rsp;  //给客户端的响应
        auto err_response = [&req, &rsp, &response](const std::string &errmsg) -> void {
            rsp.set_success(false);
            rsp.set_errmsg(errmsg);
            response.set_content(rsp.SerializeAsString(), "application/x-protbuf");
        };
        bool ret = req.ParseFromString(request.body);
        if(ret == false) {
            LOG_ERROR("设置会话名称请求正文反序列化失败");
            return err_response("设置会话名称请求正文反序列化失败");
        }
        //2. 客户端身份识别和鉴权
        std::string ssid = req.session_id();
        auto uid = _redis_session->uid(ssid);
        if(!uid) {
            LOG_ERROR("请求ID - {} 获取登录会话 {} 关联用户信息失败", req.request_id(), ssid);
            return err_response("获取登录会话关联用户信息失败");
        }
        req.set_user_id(*uid);
        //3. 将请求转发给消息转发子服务进行业务处理
        auto channel = _mm_channels->choose(_chatsession_service_name);
        if(!channel) {
            LOG_ERROR("请求ID - {} 未找到可提供业务的会话管理子服务节点", req.request_id());
            return err_response("未找到可提供业务的会话管理子服务节点");
        }
        ChatSessionService_Stub stub(channel.get());
        brpc::Controller cntl;
        stub.SetChatSessionName(&cntl, &req, &rsp, nullptr);
        if(cntl.Failed()) {
            LOG_ERROR("请求ID - {} 会话管理子服务调用失败: {}", req.request_id(), cntl.ErrorText());
            return err_response("会话管理子服务调用失败");
        }
        //5. 向客户端进行响应
        response.set_content(rsp.SerializeAsString(), "application/x-protbuf");
    }
    void SetChatSessionAvatar(const httplib::Request &request, httplib::Response &response) {
        //1. 正文反序列化，提取关键要素：登录会话ID
        SetChatSessionAvatarReq req;
        SetChatSessionAvatarRsp rsp;  //给客户端的响应
        auto err_response = [&req, &rsp, &response](const std::string &errmsg) -> void {
            rsp.set_success(false);
            rsp.set_errmsg(errmsg);
            response.set_content(rsp.SerializeAsString(), "application/x-protbuf");
        };
        bool ret = req.ParseFromString(request.body);
        if(ret == false) {
            LOG_ERROR("设置会话头像请求正文反序列化失败");
            return err_response("设置会话头像请求正文反序列化失败");
        }
        //2. 客户端身份识别和鉴权
        std::string ssid = req.session_id();
        auto uid = _redis_session->uid(ssid);
        if(!uid) {
            LOG_ERROR("请求ID - {} 获取登录会话 {} 关联用户信息失败", req.request_id(), ssid);
            return err_response("获取登录会话关联用户信息失败");
        }
        req.set_user_id(*uid);
        //3. 将请求转发给消息转发子服务进行业务处理
        auto channel = _mm_channels->choose(_chatsession_service_name);
        if(!channel) {
            LOG_ERROR("请求ID - {} 未找到可提供业务的会话管理子服务节点", req.request_id());
            return err_response("未找到可提供业务的会话管理子服务节点");
        }
        ChatSessionService_Stub stub(channel.get());
        brpc::Controller cntl;
        stub.SetChatSessionAvatar(&cntl, &req, &rsp, nullptr);
        if(cntl.Failed()) {
            LOG_ERROR("请求ID - {} 会话管理子服务调用失败: {}", req.request_id(), cntl.ErrorText());
            return err_response("会话管理子服务调用失败");
        }
        //5. 向客户端进行响应
        response.set_content(rsp.SerializeAsString(), "application/x-protbuf");
    }
    void AddChatSessionMember(const httplib::Request &request, httplib::Response &response) {
        //1. 正文反序列化，提取关键要素：登录会话ID
        AddChatSessionMemberReq req;
        AddChatSessionMemberRsp rsp;  //给客户端的响应
        auto err_response = [&req, &rsp, &response](const std::string &errmsg) -> void {
            rsp.set_success(false);
            rsp.set_errmsg(errmsg);
            response.set_content(rsp.SerializeAsString(), "application/x-protbuf");
        };
        bool ret = req.ParseFromString(request.body);
        if(ret == false) {
            LOG_ERROR("添加会话成员请求正文反序列化失败");
            return err_response("添加会话成员请求正文反序列化失败");
        }
        //2. 客户端身份识别和鉴权
        std::string ssid = req.session_id();
        auto uid = _redis_session->uid(ssid);
        if(!uid) {
            LOG_ERROR("请求ID - {} 获取登录会话 {} 关联用户信息失败", req.request_id(), ssid);
            return err_response("获取登录会话关联用户信息失败");
        }
        req.set_user_id(*uid);
        //3. 将请求转发给消息转发子服务进行业务处理
        auto channel = _mm_channels->choose(_chatsession_service_name);
        if(!channel) {
            LOG_ERROR("请求ID - {} 未找到可提供业务的会话管理子服务节点", req.request_id());
            return err_response("未找到可提供业务的会话管理子服务节点");
        }
        ChatSessionService_Stub stub(channel.get());
        brpc::Controller cntl;
        stub.AddChatSessionMember(&cntl, &req, &rsp, nullptr);
        if(cntl.Failed()) {
            LOG_ERROR("请求ID - {} 会话管理子服务调用失败: {}", req.request_id(), cntl.ErrorText());
            return err_response("会话管理子服务调用失败");
        }
        //5. 向客户端进行响应
        response.set_content(rsp.SerializeAsString(), "application/x-protbuf");
    }
    void RemoveChatSessionMember(const httplib::Request &request, httplib::Response &response) {
        //1. 正文反序列化，提取关键要素：登录会话ID
        RemoveChatSessionMemberReq req;
        RemoveChatSessionMemberRsp rsp;  //给客户端的响应
        auto err_response = [&req, &rsp, &response](const std::string &errmsg) -> void {
            rsp.set_success(false);
            rsp.set_errmsg(errmsg);
            response.set_content(rsp.SerializeAsString(), "application/x-protbuf");
        };
        bool ret = req.ParseFromString(request.body);
        if(ret == false) {
            LOG_ERROR("移除会话成员请求正文反序列化失败");
            return err_response("移除会话成员请求正文反序列化失败");
        }
        //2. 客户端身份识别和鉴权
        std::string ssid = req.session_id();
        auto uid = _redis_session->uid(ssid);
        if(!uid) {
            LOG_ERROR("请求ID - {} 获取登录会话 {} 关联用户信息失败", req.request_id(), ssid);
            return err_response("获取登录会话关联用户信息失败");
        }
        req.set_user_id(*uid);
        //3. 将请求转发给消息转发子服务进行业务处理
        auto channel = _mm_channels->choose(_chatsession_service_name);
        if(!channel) {
            LOG_ERROR("请求ID - {} 未找到可提供业务的会话管理子服务节点", req.request_id());
            return err_response("未找到可提供业务的会话管理子服务节点");
        }
        ChatSessionService_Stub stub(channel.get());
        brpc::Controller cntl;
        stub.RemoveChatSessionMember(&cntl, &req, &rsp, nullptr);
        if(cntl.Failed()) {
            LOG_ERROR("请求ID - {} 会话管理子服务调用失败: {}", req.request_id(), cntl.ErrorText());
            return err_response("会话管理子服务调用失败");
        }
        //5. 向客户端进行响应
        response.set_content(rsp.SerializeAsString(), "application/x-protbuf");
    }
    void TransferChatSessionOwner(const httplib::Request &request, httplib::Response &response) {
        //1. 正文反序列化，提取关键要素：登录会话ID
        TransferChatSessionOwnerReq req;
        TransferChatSessionOwnerRsp rsp;  //给客户端的响应
        auto err_response = [&req, &rsp, &response](const std::string &errmsg) -> void {
            rsp.set_success(false);
            rsp.set_errmsg(errmsg);
            response.set_content(rsp.SerializeAsString(), "application/x-protbuf");
        };
        bool ret = req.ParseFromString(request.body);
        if(ret == false) {
            LOG_ERROR("转让群主请求正文反序列化失败");
            return err_response("转让群主请求正文反序列化失败");
        }
        //2. 客户端身份识别和鉴权
        std::string ssid = req.session_id();
        auto uid = _redis_session->uid(ssid);
        if(!uid) {
            LOG_ERROR("请求ID - {} 获取登录会话 {} 关联用户信息失败", req.request_id(), ssid);
            return err_response("获取登录会话关联用户信息失败");
        }
        req.set_user_id(*uid);
        //3. 将请求转发给消息转发子服务进行业务处理
        auto channel = _mm_channels->choose(_chatsession_service_name);
        if(!channel) {
            LOG_ERROR("请求ID - {} 未找到可提供业务的会话管理子服务节点", req.request_id());
            return err_response("未找到可提供业务的会话管理子服务节点");
        }
        ChatSessionService_Stub stub(channel.get());
        brpc::Controller cntl;
        stub.TransferChatSessionOwner(&cntl, &req, &rsp, nullptr);
        if(cntl.Failed()) {
            LOG_ERROR("请求ID - {} 会话管理子服务调用失败: {}", req.request_id(), cntl.ErrorText());
            return err_response("会话管理子服务调用失败");
        }
        //5. 向客户端进行响应
        response.set_content(rsp.SerializeAsString(), "application/x-protbuf");
    }
    void ModifyMemberPermission(const httplib::Request &request, httplib::Response &response) {
        //1. 正文反序列化，提取关键要素：登录会话ID
        ModifyMemberPermissionReq req;
        ModifyMemberPermissionRsp rsp;  //给客户端的响应
        auto err_response = [&req, &rsp, &response](const std::string &errmsg) -> void {
            rsp.set_success(false);
            rsp.set_errmsg(errmsg);
            response.set_content(rsp.SerializeAsString(), "application/x-protbuf");
        };
        bool ret = req.ParseFromString(request.body);
        if(ret == false) {
            LOG_ERROR("编辑会话成员权限请求正文反序列化失败");
            return err_response("编辑会话成员权限请求正文反序列化失败");
        }
        //2. 客户端身份识别和鉴权
        std::string ssid = req.session_id();
        auto uid = _redis_session->uid(ssid);
        if(!uid) {
            LOG_ERROR("请求ID - {} 获取登录会话 {} 关联用户信息失败", req.request_id(), ssid);
            return err_response("获取登录会话关联用户信息失败");
        }
        req.set_user_id(*uid);
        //3. 将请求转发给消息转发子服务进行业务处理
        auto channel = _mm_channels->choose(_chatsession_service_name);
        if(!channel) {
            LOG_ERROR("请求ID - {} 未找到可提供业务的会话管理子服务节点", req.request_id());
            return err_response("未找到可提供业务的会话管理子服务节点");
        }
        ChatSessionService_Stub stub(channel.get());
        brpc::Controller cntl;
        stub.ModifyMemberPermission(&cntl, &req, &rsp, nullptr);
        if(cntl.Failed()) {
            LOG_ERROR("请求ID - {} 会话管理子服务调用失败: {}", req.request_id(), cntl.ErrorText());
            return err_response("会话管理子服务调用失败");
        }
        //5. 向客户端进行响应
        response.set_content(rsp.SerializeAsString(), "application/x-protbuf");
    }
    void ModifyChatSessionStatus(const httplib::Request &request, httplib::Response &response) {
        //1. 正文反序列化，提取关键要素：登录会话ID
        ModifyChatSessionStatusReq req;
        ModifyChatSessionStatusRsp rsp;  //给客户端的响应
        auto err_response = [&req, &rsp, &response](const std::string &errmsg) -> void {
            rsp.set_success(false);
            rsp.set_errmsg(errmsg);
            response.set_content(rsp.SerializeAsString(), "application/x-protbuf");
        };
        bool ret = req.ParseFromString(request.body);
        if(ret == false) {
            LOG_ERROR("编辑会话成员权限请求正文反序列化失败");
            return err_response("编辑会话成员权限请求正文反序列化失败");
        }
        //2. 客户端身份识别和鉴权
        std::string ssid = req.session_id();
        auto uid = _redis_session->uid(ssid);
        if(!uid) {
            LOG_ERROR("请求ID - {} 获取登录会话 {} 关联用户信息失败", req.request_id(), ssid);
            return err_response("获取登录会话关联用户信息失败");
        }
        req.set_user_id(*uid);
        //3. 将请求转发给消息转发子服务进行业务处理
        auto channel = _mm_channels->choose(_chatsession_service_name);
        if(!channel) {
            LOG_ERROR("请求ID - {} 未找到可提供业务的会话管理子服务节点", req.request_id());
            return err_response("未找到可提供业务的会话管理子服务节点");
        }
        ChatSessionService_Stub stub(channel.get());
        brpc::Controller cntl;
        stub.ModifyChatSessionStatus(&cntl, &req, &rsp, nullptr);
        if(cntl.Failed()) {
            LOG_ERROR("请求ID - {} 会话管理子服务调用失败: {}", req.request_id(), cntl.ErrorText());
            return err_response("会话管理子服务调用失败");
        }
        //5. 向客户端进行响应
        response.set_content(rsp.SerializeAsString(), "application/x-protbuf");
    }
    void SearchChatSession(const httplib::Request &request, httplib::Response &response) {
        //1. 正文反序列化，提取关键要素：登录会话ID
        SearchChatSessionReq req;
        SearchChatSessionRsp rsp;  //给客户端的响应
        auto err_response = [&req, &rsp, &response](const std::string &errmsg) -> void {
            rsp.set_success(false);
            rsp.set_errmsg(errmsg);
            response.set_content(rsp.SerializeAsString(), "application/x-protbuf");
        };
        bool ret = req.ParseFromString(request.body);
        if(ret == false) {
            LOG_ERROR("搜索会话请求正文反序列化失败");
            return err_response("搜索会话请求正文反序列化失败");
        }
        //2. 客户端身份识别和鉴权
        std::string ssid = req.session_id();
        auto uid = _redis_session->uid(ssid);
        if(!uid) {
            LOG_ERROR("请求ID - {} 获取登录会话 {} 关联用户信息失败", req.request_id(), ssid);
            return err_response("获取登录会话关联用户信息失败");
        }
        req.set_user_id(*uid);
        //3. 将请求转发给消息转发子服务进行业务处理
        auto channel = _mm_channels->choose(_chatsession_service_name);
        if(!channel) {
            LOG_ERROR("请求ID - {} 未找到可提供业务的会话管理子服务节点", req.request_id());
            return err_response("未找到可提供业务的会话管理子服务节点");
        }
        ChatSessionService_Stub stub(channel.get());
        brpc::Controller cntl;
        stub.SearchChatSession(&cntl, &req, &rsp, nullptr);
        if(cntl.Failed()) {
            LOG_ERROR("请求ID - {} 会话管理子服务调用失败: {}", req.request_id(), cntl.ErrorText());
            return err_response("会话管理子服务调用失败");
        }
        //5. 向客户端进行响应
        response.set_content(rsp.SerializeAsString(), "application/x-protbuf");
    }
    void SetSessionMuted(const httplib::Request &request, httplib::Response &response) {
        //1. 正文反序列化，提取关键要素：登录会话ID
        SetSessionMutedReq req;
        SetSessionMutedRsp rsp;  //给客户端的响应
        auto err_response = [&req, &rsp, &response](const std::string &errmsg) -> void {
            rsp.set_success(false);
            rsp.set_errmsg(errmsg);
            response.set_content(rsp.SerializeAsString(), "application/x-protbuf");
        };
        bool ret = req.ParseFromString(request.body);
        if(ret == false) {
            LOG_ERROR("设置会话免打扰请求正文反序列化失败");
            return err_response("设置会话免打扰请求正文反序列化失败");
        }
        //2. 客户端身份识别和鉴权
        std::string ssid = req.session_id();
        auto uid = _redis_session->uid(ssid);
        if(!uid) {
            LOG_ERROR("请求ID - {} 获取登录会话 {} 关联用户信息失败", req.request_id(), ssid);
            return err_response("获取登录会话关联用户信息失败");
        }
        req.set_user_id(*uid);
        //3. 将请求转发给消息转发子服务进行业务处理
        auto channel = _mm_channels->choose(_chatsession_service_name);
        if(!channel) {
            LOG_ERROR("请求ID - {} 未找到可提供业务的会话管理子服务节点", req.request_id());
            return err_response("未找到可提供业务的会话管理子服务节点");
        }
        ChatSessionService_Stub stub(channel.get());
        brpc::Controller cntl;
        stub.SetSessionMuted(&cntl, &req, &rsp, nullptr);
        if(cntl.Failed()) {
            LOG_ERROR("请求ID - {} 会话管理子服务调用失败: {}", req.request_id(), cntl.ErrorText());
            return err_response("会话管理子服务调用失败");
        }
        //5. 向客户端进行响应
        response.set_content(rsp.SerializeAsString(), "application/x-protbuf");
    }
    void SetSessionPinned(const httplib::Request &request, httplib::Response &response) {
        //1. 正文反序列化，提取关键要素：登录会话ID
        SetSessionPinnedReq req;
        SetSessionPinnedRsp rsp;  //给客户端的响应
        auto err_response = [&req, &rsp, &response](const std::string &errmsg) -> void {
            rsp.set_success(false);
            rsp.set_errmsg(errmsg);
            response.set_content(rsp.SerializeAsString(), "application/x-protbuf");
        };
        bool ret = req.ParseFromString(request.body);
        if(ret == false) {
            LOG_ERROR("设置会话置顶请求正文反序列化失败");
            return err_response("设置会话置顶请求正文反序列化失败");
        }
        //2. 客户端身份识别和鉴权
        std::string ssid = req.session_id();
        auto uid = _redis_session->uid(ssid);
        if(!uid) {
            LOG_ERROR("请求ID - {} 获取登录会话 {} 关联用户信息失败", req.request_id(), ssid);
            return err_response("获取登录会话关联用户信息失败");
        }
        req.set_user_id(*uid);
        //3. 将请求转发给消息转发子服务进行业务处理
        auto channel = _mm_channels->choose(_chatsession_service_name);
        if(!channel) {
            LOG_ERROR("请求ID - {} 未找到可提供业务的会话管理子服务节点", req.request_id());
            return err_response("未找到可提供业务的会话管理子服务节点");
        }
        ChatSessionService_Stub stub(channel.get());
        brpc::Controller cntl;
        stub.SetSessionPinned(&cntl, &req, &rsp, nullptr);
        if(cntl.Failed()) {
            LOG_ERROR("请求ID - {} 会话管理子服务调用失败: {}", req.request_id(), cntl.ErrorText());
            return err_response("会话管理子服务调用失败");
        }
        //5. 向客户端进行响应
        response.set_content(rsp.SerializeAsString(), "application/x-protbuf");
    }
    void SetSessionVisible(const httplib::Request &request, httplib::Response &response) {
        //1. 正文反序列化，提取关键要素：登录会话ID
        SetSessionVisibleReq req;
        SetSessionVisibleRsp rsp;  //给客户端的响应
        auto err_response = [&req, &rsp, &response](const std::string &errmsg) -> void {
            rsp.set_success(false);
            rsp.set_errmsg(errmsg);
            response.set_content(rsp.SerializeAsString(), "application/x-protbuf");
        };
        bool ret = req.ParseFromString(request.body);
        if(ret == false) {
            LOG_ERROR("设置会话隐藏请求正文反序列化失败");
            return err_response("设置会话隐藏请求正文反序列化失败");
        }
        //2. 客户端身份识别和鉴权
        std::string ssid = req.session_id();
        auto uid = _redis_session->uid(ssid);
        if(!uid) {
            LOG_ERROR("请求ID - {} 获取登录会话 {} 关联用户信息失败", req.request_id(), ssid);
            return err_response("获取登录会话关联用户信息失败");
        }
        req.set_user_id(*uid);
        //3. 将请求转发给消息转发子服务进行业务处理
        auto channel = _mm_channels->choose(_chatsession_service_name);
        if(!channel) {
            LOG_ERROR("请求ID - {} 未找到可提供业务的会话管理子服务节点", req.request_id());
            return err_response("未找到可提供业务的会话管理子服务节点");
        }
        ChatSessionService_Stub stub(channel.get());
        brpc::Controller cntl;
        stub.SetSessionVisible(&cntl, &req, &rsp, nullptr);
        if(cntl.Failed()) {
            LOG_ERROR("请求ID - {} 会话管理子服务调用失败: {}", req.request_id(), cntl.ErrorText());
            return err_response("会话管理子服务调用失败");
        }
        //5. 向客户端进行响应
        response.set_content(rsp.SerializeAsString(), "application/x-protbuf");
    }
    void QuitChatSession(const httplib::Request &request, httplib::Response &response) {
        //1. 正文反序列化，提取关键要素：登录会话ID
        QuitChatSessionReq req;
        QuitChatSessionRsp rsp;  //给客户端的响应
        auto err_response = [&req, &rsp, &response](const std::string &errmsg) -> void {
            rsp.set_success(false);
            rsp.set_errmsg(errmsg);
            response.set_content(rsp.SerializeAsString(), "application/x-protbuf");
        };
        bool ret = req.ParseFromString(request.body);
        if(ret == false) {
            LOG_ERROR("退出会话请求正文反序列化失败");
            return err_response("退出会话请求正文反序列化失败");
        }
        //2. 客户端身份识别和鉴权
        std::string ssid = req.session_id();
        auto uid = _redis_session->uid(ssid);
        if(!uid) {
            LOG_ERROR("请求ID - {} 获取登录会话 {} 关联用户信息失败", req.request_id(), ssid);
            return err_response("获取登录会话关联用户信息失败");
        }
        req.set_user_id(*uid);
        //3. 将请求转发给消息转发子服务进行业务处理
        auto channel = _mm_channels->choose(_chatsession_service_name);
        if(!channel) {
            LOG_ERROR("请求ID - {} 未找到可提供业务的会话管理子服务节点", req.request_id());
            return err_response("未找到可提供业务的会话管理子服务节点");
        }
        ChatSessionService_Stub stub(channel.get());
        brpc::Controller cntl;
        stub.QuitChatSession(&cntl, &req, &rsp, nullptr);
        if(cntl.Failed()) {
            LOG_ERROR("请求ID - {} 会话管理子服务调用失败: {}", req.request_id(), cntl.ErrorText());
            return err_response("会话管理子服务调用失败");
        }
        //5. 向客户端进行响应
        response.set_content(rsp.SerializeAsString(), "application/x-protbuf");
    }
    void MsgReadAck(const httplib::Request &request, httplib::Response &response) {
        //1. 正文反序列化，提取关键要素：登录会话ID
        MsgReadAckReq req;
        MsgReadAckRsp rsp;  //给客户端的响应
        auto err_response = [&req, &rsp, &response](const std::string &errmsg) -> void {
            rsp.set_success(false);
            rsp.set_errmsg(errmsg);
            response.set_content(rsp.SerializeAsString(), "application/x-protbuf");
        };
        bool ret = req.ParseFromString(request.body);
        if(ret == false) {
            LOG_ERROR("确认最新已读消息正文反序列化失败");
            return err_response("确认最新已读消息正文反序列化失败");
        }
        //2. 客户端身份识别和鉴权
        std::string ssid = req.session_id();
        auto uid = _redis_session->uid(ssid);
        if(!uid) {
            LOG_ERROR("请求ID - {} 获取登录会话 {} 关联用户信息失败", req.request_id(), ssid);
            return err_response("获取登录会话关联用户信息失败");
        }
        req.set_user_id(*uid);
        //3. 将请求转发给消息转发子服务进行业务处理
        auto channel = _mm_channels->choose(_chatsession_service_name);
        if(!channel) {
            LOG_ERROR("请求ID - {} 未找到可提供业务的会话管理子服务节点", req.request_id());
            return err_response("未找到可提供业务的会话管理子服务节点");
        }
        ChatSessionService_Stub stub(channel.get());
        brpc::Controller cntl;
        stub.MsgReadAck(&cntl, &req, &rsp, nullptr);
        if(cntl.Failed()) {
            LOG_ERROR("请求ID - {} 会话管理子服务调用失败: {}", req.request_id(), cntl.ErrorText());
            return err_response("会话管理子服务调用失败");
        }
        //5. 向客户端进行响应
        response.set_content(rsp.SerializeAsString(), "application/x-protbuf");
    }
    void GetMemberIdList(const httplib::Request &request, httplib::Response &response) {
        //1. 正文反序列化，提取关键要素：登录会话ID
        GetMemberIdListReq req;
        GetMemberIdListRsp rsp;  //给客户端的响应
        auto err_response = [&req, &rsp, &response](const std::string &errmsg) -> void {
            rsp.set_success(false);
            rsp.set_errmsg(errmsg);
            response.set_content(rsp.SerializeAsString(), "application/x-protbuf");
        };
        bool ret = req.ParseFromString(request.body);
        if(ret == false) {
            LOG_ERROR("获取会话成员ID正文反序列化失败");
            return err_response("获取会话成员ID正文反序列化失败");
        }
        //2. 客户端身份识别和鉴权
        std::string ssid = req.session_id();
        auto uid = _redis_session->uid(ssid);
        if(!uid) {
            LOG_ERROR("请求ID - {} 获取登录会话 {} 关联用户信息失败", req.request_id(), ssid);
            return err_response("获取登录会话关联用户信息失败");
        }
        req.set_user_id(*uid);
        //3. 将请求转发给消息转发子服务进行业务处理
        auto channel = _mm_channels->choose(_chatsession_service_name);
        if(!channel) {
            LOG_ERROR("请求ID - {} 未找到可提供业务的会话管理子服务节点", req.request_id());
            return err_response("未找到可提供业务的会话管理子服务节点");
        }
        ChatSessionService_Stub stub(channel.get());
        brpc::Controller cntl;
        stub.GetMemberIdList(&cntl, &req, &rsp, nullptr);
        if(cntl.Failed()) {
            LOG_ERROR("请求ID - {} 会话管理子服务调用失败: {}", req.request_id(), cntl.ErrorText());
            return err_response("会话管理子服务调用失败");
        }
        //5. 向客户端进行响应
        response.set_content(rsp.SerializeAsString(), "application/x-protbuf");
    }
    void GetOfflineMsg(const httplib::Request &request, httplib::Response &response) {
        //1. 正文反序列化，提取关键要素：登录会话ID
        GetOfflineMsgReq req;
        GetOfflineMsgRsp rsp;  //给客户端的响应
        auto err_response = [&req, &rsp, &response](const std::string &errmsg) -> void {
            rsp.set_success(false);
            rsp.set_errmsg(errmsg);
            response.set_content(rsp.SerializeAsString(), "application/x-protbuf");
        };
        bool ret = req.ParseFromString(request.body);
        if(ret == false) {
            LOG_ERROR("获取离线消息请求正文反序列化失败");
            return err_response("获取离线消息请求正文反序列化失败");
        }
        //2. 客户端身份识别和鉴权
        std::string ssid = req.session_id();
        auto uid = _redis_session->uid(ssid);
        if(!uid) {
            LOG_ERROR("请求ID - {} 获取登录会话 {} 关联用户信息失败", req.request_id(), ssid);
            return err_response("获取登录会话关联用户信息失败");
        }
        req.set_user_id(*uid);
        //3. 将请求转发给消息转发子服务进行业务处理
        auto channel = _mm_channels->choose(_chatsession_service_name);
        if(!channel) {
            LOG_ERROR("请求ID - {} 未找到可提供业务的会话管理子服务节点", req.request_id());
            return err_response("未找到可提供业务的会话管理子服务节点");
        }
        MsgStorageService_Stub stub(channel.get());
        brpc::Controller cntl;
        stub.GetOfflineMsg(&cntl, &req, &rsp, nullptr);
        if(cntl.Failed()) {
            LOG_ERROR("请求ID - {} 会话管理子服务调用失败: {}", req.request_id(), cntl.ErrorText());
            return err_response("会话管理子服务调用失败");
        }
        //5. 向客户端进行响应
        response.set_content(rsp.SerializeAsString(), "application/x-protbuf");
    }
    void GetUnreadCount(const httplib::Request &request, httplib::Response &response) {
        //1. 正文反序列化，提取关键要素：登录会话ID
        GetUnreadCountReq req;
        GetUnreadCountRsp rsp;  //给客户端的响应
        auto err_response = [&req, &rsp, &response](const std::string &errmsg) -> void {
            rsp.set_success(false);
            rsp.set_errmsg(errmsg);
            response.set_content(rsp.SerializeAsString(), "application/x-protbuf");
        };
        bool ret = req.ParseFromString(request.body);
        if(ret == false) {
            LOG_ERROR("获取未读消息数请求正文反序列化失败");
            return err_response("获取未读消息数请求正文反序列化失败");
        }
        //2. 客户端身份识别和鉴权
        std::string ssid = req.session_id();
        auto uid = _redis_session->uid(ssid);
        if(!uid) {
            LOG_ERROR("请求ID - {} 获取登录会话 {} 关联用户信息失败", req.request_id(), ssid);
            return err_response("获取登录会话关联用户信息失败");
        }
        req.set_user_id(*uid);
        //3. 将请求转发给消息转发子服务进行业务处理
        auto channel = _mm_channels->choose(_chatsession_service_name);
        if(!channel) {
            LOG_ERROR("请求ID - {} 未找到可提供业务的会话管理子服务节点", req.request_id());
            return err_response("未找到可提供业务的会话管理子服务节点");
        }
        MsgStorageService_Stub stub(channel.get());
        brpc::Controller cntl;
        stub.GetUnreadCount(&cntl, &req, &rsp, nullptr);
        if(cntl.Failed()) {
            LOG_ERROR("请求ID - {} 会话管理子服务调用失败: {}", req.request_id(), cntl.ErrorText());
            return err_response("会话管理子服务调用失败");
        }
        //5. 向客户端进行响应
        response.set_content(rsp.SerializeAsString(), "application/x-protbuf");
    }
private:
    Session::ptr _redis_session;

    std::string _speech_service_name;
    std::string _file_service_name;
    std::string _user_service_name;
    std::string _transmite_service_name;
    std::string _message_service_name;
    std::string _friend_service_name;
    std::string _chatsession_service_name;
    std::string _push_service_name;
    ServiceManager::ptr _mm_channels;
    Discovery::ptr _service_discoverer;

    int _http_port {0};
    httplib::Server _http_server;
};

class GatewayServerBuilder
{
public:
    /* brief: 构造redis客户端对象 */
    void make_redis_object(const std::string &host,
                        uint16_t port,
                        int db,
                        bool keep_alive)
    {
        _redis_client = RedisClientFactory::create(host, port, db, keep_alive);
    }
    /* brief: 用于构造服务发现&信道管理客户端对象 */
    void make_discovery_object(const std::string &reg_host,
                            const std::string &base_service_name,
                            const std::string &speech_service_name,
                            const std::string &file_service_name,
                            const std::string &user_service_name,
                            const std::string &transmite_service_name,
                            const std::string &message_service_name,
                            const std::string &friend_service_name,
                            const std::string &chatsession_service_name,
                            const std::string &push_service_name = "/service/push_service")
    {
        _speech_service_name = speech_service_name;
        _file_service_name = file_service_name;
        _user_service_name = user_service_name;
        _transmite_service_name = transmite_service_name;
        _message_service_name = message_service_name;
        _friend_service_name = friend_service_name;
        _chatsession_service_name = chatsession_service_name;
        _push_service_name = push_service_name;

        _mm_channels = std::make_shared<ServiceManager>();
        _mm_channels->declared(_speech_service_name);
        _mm_channels->declared(_file_service_name);
        _mm_channels->declared(_user_service_name);
        _mm_channels->declared(_transmite_service_name);
        _mm_channels->declared(_message_service_name);
        _mm_channels->declared(_friend_service_name);
        _mm_channels->declared(_chatsession_service_name);
        _mm_channels->declared(_push_service_name);

        auto put_cb = std::bind(&ServiceManager::onServiceOnline, _mm_channels.get(), std::placeholders::_1, std::placeholders::_2);
        auto del_cb = std::bind(&ServiceManager::onServiceOffline, _mm_channels.get(), std::placeholders::_1, std::placeholders::_2);

        _service_discoverer = std::make_shared<Discovery>(reg_host, base_service_name, put_cb, del_cb);
    }
    /* brief: B3 — Gateway 仅监听 HTTP；WebSocket 由 push 服务终结 */
    void make_server_object(int http_port)
    {
        _http_port = http_port;
    }
    GatewayServer::ptr build() {
        if(!_redis_client) {
            LOG_ERROR("还未初始化 Redis 客户端模块");
            abort();
        }
        if(!_service_discoverer) {
            LOG_ERROR("还未初始化服务发现模块");
            abort();
        } 
        if(!_mm_channels) {
            LOG_ERROR("还未初始化信道管理模块");
            abort();
        }
        GatewayServer::ptr server = std::make_shared<GatewayServer>(_http_port,
                                                                _redis_client,
                                                                _mm_channels,
                                                                _service_discoverer,
                                                                _speech_service_name,
                                                                _file_service_name,
                                                                _user_service_name,
                                                                _transmite_service_name,
                                                                _message_service_name,
                                                                _friend_service_name,
                                                                _chatsession_service_name,
                                                                _push_service_name);
        return server;
    }
private:
    int _http_port;

    std::shared_ptr<sw::redis::Redis> _redis_client;

    std::string _speech_service_name;
    std::string _file_service_name;
    std::string _user_service_name;
    std::string _transmite_service_name;
    std::string _message_service_name;
    std::string _friend_service_name;
    std::string _chatsession_service_name;
    std::string _push_service_name;
    ServiceManager::ptr _mm_channels;
    Discovery::ptr _service_discoverer;
};

}
