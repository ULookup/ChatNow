#pragma once

#include <brpc/server.h>
#include <butil/logging.h>

#include "httplib.h"

#include "data_redis.hpp"
#include "etcd.hpp"
#include "logger.hpp"
#include "channel.hpp"

#include "connection.hpp"

/* protobuf 框架代码 */
#include "base.pb.h"
#include "speech.pb.h"
#include "file.pb.h"
#include "user.pb.h"
#include "transmite.pb.h"
#include "message.pb.h"
#include "friend.pb.h"
#include "gateway.pb.h"
#include "notify.pb.h"

namespace chatnow
{

#define GET_MAIL_VERIFY_CODE        "/server/user/get_mail_verify_code"
#define USERNAME_REGISTER           "/service/user/username_register"             //用户名密码注册 
#define USERNAME_LOGIN              "/service/user/username_login"                //用户名密码登录 
#define MAIL_REGISTER               "/service/user/mail_register"                 //邮箱号码注册 
#define MAIL_LOGIN                  "/service/user/mail_login"                    //邮箱号码登录 
#define GET_USER_INFO               "/service/user/get_user_info"                 //获取个人信息 
#define SET_AVATAR                  "/service/user/set_avatar"                    //修改头像 
#define SET_NICKNAME                "/service/user/set_nickname"                  //修改昵称 
#define SET_DESCRIPTION             "/service/user/set_description"               //修改签名 
#define SET_MAIL                    "/service/user/set_mail"                      //修改绑定邮箱 
#define GET_FRIEND_LIST             "/service/friend/get_friend_list"             //获取好友列表 
#define GET_FRIEND_INFO             "/service/friend/get_friend_info"             //获取好友信息 
#define ADD_FRIEND_APPLY            "/service/friend/add_friend_apply"            //发送好友申请 
#define ADD_FRIEND_PROCESS          "/service/friend/add_friend_process"          //好友申请处理 
#define REMOVE_FRIEND               "/service/friend/remove_friend"               //删除好友 
#define SEARCH_FRIEND               "/service/friend/search_friend"               //搜索用户 
#define GET_CHAT_SESSION_LIST       "/service/friend/get_chat_session_list"       //获取指定用户的消息会话列表 
#define CREATE_CHAT_SESSION         "/service/friend/create_chat_session"         //创建消息会话 
#define GET_CHAT_SESSION_MEMBER     "/service/friend/get_chat_session_member"     //获取消息会话成员列表 
#define GET_PENDING_FRIEND_EVENTS   "/service/friend/get_pending_friend_events"   //获取待处理好友申请事件列表 
#define GET_HISTORY                 "/service/message_storage/get_history"        //获取历史消息/离线消息列表 
#define GET_RECENT                  "/service/message_storage/get_recent"         //获取最近 N 条消息列表 
#define SEARCH_HISTORY              "/service/message_storage/search_history"     //搜索历史消息 
#define NEW_MESSAGE                 "/service/message_transmit/new_message"       //发送消息 
#define GET_SINGLE_FILE             "/service/file/get_single_file"               //获取单个文件数据 
#define GET_MULTI_FILE              "/service/file/get_multi_file"                //获取多个文件数据 
#define PUT_SINGLE_FILE             "/service/file/put_single_file"               //发送单个文件 
#define PUT_MULTI_FILE              "/service/file/put_multi_file"                //发送多个文件 
#define RECOGNITION                 "/service/speech/recognition"                 //语音转文字

class GatewayServer
{
public:
    using ptr = std::shared_ptr<GatewayServer>;

    GatewayServer(int websocket_port, 
                int http_port,
                const std::shared_ptr<sw::redis::Redis> &redis_client,
                const ServiceManager::ptr &channels,
                const Discovery::ptr &service_discoverer,
                const std::string &speech_service_name,
                const std::string &file_service_name,
                const std::string &user_service_name,
                const std::string &transmite_service_name,
                const std::string &message_service_name,
                const std::string &friend_service_name)
        : _redis_session(std::make_shared<Session>(redis_client)),
        _redis_status(std::make_shared<Status>(redis_client)),
        _mm_channels(channels),
        _service_discoverer(service_discoverer),
        _speech_service_name(speech_service_name),
        _file_service_name(file_service_name),
        _user_service_name(user_service_name),
        _transmite_service_name(transmite_service_name),
        _message_service_name(message_service_name),
        _friend_service_name(friend_service_name),
        _connections(std::make_shared<Connection>())
    {
        //1. 搭建websocket服务器
        _ws_server.set_access_channels(websocketpp::log::alevel::none);
        _ws_server.init_asio();
        _ws_server.set_open_handler(std::bind(&GatewayServer::onOpen, this, std::placeholders::_1));
        _ws_server.set_close_handler(std::bind(&GatewayServer::onClose, this, std::placeholders::_1));
        auto ws_callback = std::bind(&GatewayServer::onMessage, this, std::placeholders::_1, std::placeholders::_2);
        _ws_server.set_message_handler(ws_callback);
        _ws_server.set_reuse_addr(true);
        _ws_server.listen(websocket_port);
        _ws_server.start_accept();

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

        _http_thread = std::thread([this, http_port](){
            _http_server.listen("0.0.0.0", http_port);
        });
        _http_thread.detach();
    }
    void start() {
        _ws_server.run();
    }
private:
    void onOpen(websocketpp::connection_hdl hdl) { LOG_DEBUG("WebSocket 长连接建立成功 {}", (size_t)_ws_server.get_con_from_hdl(hdl).get()); }

    void onClose(websocketpp::connection_hdl hdl) {
        LOG_DEBUG("WebSocket 长连接断开");
        // 长连接断开时做的清理动作
        auto conn = _ws_server.get_con_from_hdl(hdl);
        //0. 通过连接对象，获取对应的用户ID与登录会话ID
        std::string uid, ssid;
        bool ret =_connections->client(conn, uid, ssid);
        if(ret == false) {
            LOG_WARN("长连接断开，未找到长连接对应的客户端信息");
            return;
        }
        //1. 移除登录会话信息
        _redis_session->remove(ssid);
        //2. 移除登录状态信息
        _redis_status->remove(uid);
        //3. 移除长连接管理数据
        _connections->remove(conn);
        LOG_DEBUG("{} - {} - {} 长连接断开，清理缓存数据", (size_t)conn.get(), uid, ssid);
    }

    void keepAlive(server_t::connection_ptr conn) {
        if(!conn || conn->get_state() !=  websocketpp::session::state::value::open) {
            LOG_DEBUG("非正常连接状态，结束连接保活");
            return;
        } 
        conn->ping("");
        _ws_server.set_timer(60000, std::bind(&GatewayServer::keepAlive, this, conn));
    }

    void onMessage(websocketpp::connection_hdl hdl, server_t::message_ptr msg) {
        // 收到第一条消息后，根据消息中的会话ID进行身份识别，将客户端长连接添加管理
        //1. 取出长连接对应的连接对象
        auto conn = _ws_server.get_con_from_hdl(hdl);
        //2. 针对消息内容进行反序列化 -- ClientAuthenticationReq -- 提取登录会话ID
        ClientAuthenticationReq request;
        bool ret = request.ParseFromString(msg->get_payload());
        if(ret == false) {
            LOG_ERROR("长连接身份识别失败：正文反序列化失败");
            _ws_server.close(hdl, websocketpp::close::status::unsupported_data, "正文反序列化失败");
            return;
        }
        //3. 在会话信息缓存中，查找会话信息
        std::string ssid = request.session_id();
        auto uid = _redis_session->uid(ssid);
        if(!uid) {
            //4. 会话信息不存在则关闭连接
            LOG_ERROR("长连接身份识别失败：未找到会话信息 {}", ssid);
            _ws_server.close(hdl, websocketpp::close::status::unsupported_data, "未找到会话信息");
            return;
        }
        //5. 会话信息存在，则添加长连接管理
        _connections->insert(conn, *uid, ssid);
        LOG_DEBUG("新增长连接管理: {} - {} - {}", (size_t)conn.get(), *uid, ssid);
        keepAlive(conn);
    }

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
        //4. 若业务处理成功 -- 且获取被申请方用户长连接成功，则向被申请方进行好友申请事件通知
        auto conn = _connections->connection(req.request_id());
        if(rsp.success() && conn) {
            LOG_DEBUG("找到被申请人 {} 长连接, 对其进行好友申请通知", req.request_id());
            auto user_rsp = _GetUserInfo(req.request_id(), *uid);
            if(!user_rsp) {
                LOG_ERROR("请求ID - {} 获取当前客户端用户信息失败", req.request_id());
                return err_response("获取当前客户端用户信息失败");
            }
            NotifyMessage notify;
            notify.set_notify_type(NotifyType::FRIEND_ADD_APPLY_NOTIFY);
            notify.mutable_friend_add_apply()->mutable_user_info()->CopyFrom(user_rsp->user_info());
            conn->send(notify.SerializeAsString(), websocketpp::frame::opcode::value::binary);
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
            auto process_conn = _connections->connection(*uid);
            auto apply_conn = _connections->connection(req.apply_user_id());
            if(apply_conn) {
                //4. 将处理结果给申请人进行通知
                NotifyMessage notify;
                notify.set_notify_type(NotifyType::FRIEND_ADD_PROCESS_NOTIFY);
                auto process_result = notify.mutable_friend_process_result();
                process_result->mutable_user_info()->CopyFrom(process_user_rsp->user_info());
                process_result->set_agree(req.agree());
                apply_conn->send(notify.SerializeAsString(), websocketpp::frame::opcode::value::binary);
            }
            //5. 若处理结果是同意，需要创建有会话的通知
            if(req.agree() && apply_conn) { // 对申请人的通知 -- 会话信息就是处理人信息
                NotifyMessage notify;
                notify.set_notify_type(NotifyType::CHAT_SESSION_CREATE_NOTIFY);
                auto chat_session = notify.mutable_new_chat_session_info();
                chat_session->mutable_chat_session_info()->set_single_chat_friend_id(*uid);
                chat_session->mutable_chat_session_info()->set_chat_session_id(rsp.new_session_id());
                chat_session->mutable_chat_session_info()->set_chat_session_name(process_user_rsp->user_info().nickname());
                chat_session->mutable_chat_session_info()->set_avatar(process_user_rsp->user_info().avatar());
                apply_conn->send(notify.SerializeAsString(), websocketpp::frame::opcode::value::binary);
            }
            if(req.agree() && process_conn) { // 对处理人的通知 -- 会话信息就是申请人信息
                NotifyMessage notify;
                notify.set_notify_type(NotifyType::CHAT_SESSION_CREATE_NOTIFY);
                auto chat_session = notify.mutable_new_chat_session_info();
                chat_session->mutable_chat_session_info()->set_single_chat_friend_id(req.apply_user_id());
                chat_session->mutable_chat_session_info()->set_chat_session_id(rsp.new_session_id());
                chat_session->mutable_chat_session_info()->set_chat_session_name(apply_user_rsp->user_info().nickname());
                chat_session->mutable_chat_session_info()->set_avatar(apply_user_rsp->user_info().avatar());
                process_conn->send(notify.SerializeAsString(), websocketpp::frame::opcode::value::binary);
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
        //4. 若业务处理成功 -- 且获取被申请方用户长连接成功，则向被申请方进行好友申请事件通知
        auto conn = _connections->connection(req.peer_id());
        if(rsp.success() && conn) {
            LOG_DEBUG("对被删除人 {} 进行好友删除通知", req.peer_id());
            NotifyMessage notify;
            notify.set_notify_type(NotifyType::FRIEND_REMOVE_NOTIFY);
            notify.mutable_friend_remove()->set_user_id(*uid);
            conn->send(notify.SerializeAsString(), websocketpp::frame::opcode::value::binary);
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
        FriendService_Stub stub(channel.get());
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
        FriendService_Stub stub(channel.get());
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
        FriendService_Stub stub(channel.get());
        brpc::Controller cntl;
        stub.ChatSessionCreate(&cntl, &req, &rsp, nullptr);
        if(cntl.Failed()) {
            LOG_ERROR("请求ID - {} 好友子服务调用失败: {}", req.request_id(), cntl.ErrorText());
            return err_response("好友子服务调用失败");
        }
        //4. 若业务处理成功 -- 且获取被申请方用户长连接成功，则向被申请方进行好友申请事件通知
        if(rsp.success()) {
            for(int i = 0; i < req.member_id_list_size(); ++i) {
                auto conn = _connections->connection(req.member_id_list(i));
                if(!conn) { continue; }
                NotifyMessage notify;
                notify.set_notify_type(NotifyType::CHAT_SESSION_CREATE_NOTIFY);
                auto chat_session = notify.mutable_new_chat_session_info();
                chat_session->mutable_chat_session_info()->CopyFrom(rsp.chat_session_info());
                conn->send(notify.SerializeAsString(), websocketpp::frame::opcode::value::binary);
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
        //4. 若业务处理成功 -- 且获取被申请方用户长连接成功，则向被申请方进行好友申请事件通知
        if(target_rsp.success()) {
            for(int i = 0; i < target_rsp.target_id_list_size(); ++i) {
                std::string notify_uid = target_rsp.target_id_list(i);
                if(notify_uid == *uid) continue; //不通知自己
                auto conn = _connections->connection(notify_uid);
                if(!conn) { continue; }
                NotifyMessage notify;
                notify.set_notify_type(NotifyType::CHAT_MESSAGE_NOTIFY);
                auto msg_info = notify.mutable_new_message_info();
                msg_info->mutable_message_info()->CopyFrom(target_rsp.message());
                conn->send(notify.SerializeAsString(), websocketpp::frame::opcode::value::binary);
            }
        }
        //5. 向客户端进行响应
        rsp.set_request_id(req.request_id());
        rsp.set_success(target_rsp.success());
        rsp.set_errmsg(target_rsp.errmsg());
        response.set_content(rsp.SerializeAsString(), "application/x-protbuf");
    }
private:
    Session::ptr _redis_session;
    Status::ptr _redis_status;

    std::string _speech_service_name;
    std::string _file_service_name;
    std::string _user_service_name;
    std::string _transmite_service_name;
    std::string _message_service_name;
    std::string _friend_service_name;
    ServiceManager::ptr _mm_channels;
    Discovery::ptr _service_discoverer;

    Connection::ptr _connections;

    server_t _ws_server;
    httplib::Server _http_server;
    std::thread _http_thread;
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
                            const std::string &friend_service_name)
    {
        _speech_service_name = speech_service_name;
        _file_service_name = file_service_name;
        _user_service_name = user_service_name;
        _transmite_service_name = transmite_service_name;
        _message_service_name = message_service_name;
        _friend_service_name = friend_service_name;

        _mm_channels = std::make_shared<ServiceManager>();
        _mm_channels->declared(_speech_service_name);
        _mm_channels->declared(_file_service_name);
        _mm_channels->declared(_user_service_name);
        _mm_channels->declared(_transmite_service_name);
        _mm_channels->declared(_message_service_name);
        _mm_channels->declared(_friend_service_name);

        auto put_cb = std::bind(&ServiceManager::onServiceOnline, _mm_channels.get(), std::placeholders::_1, std::placeholders::_2);
        auto del_cb = std::bind(&ServiceManager::onServiceOffline, _mm_channels.get(), std::placeholders::_1, std::placeholders::_2);

        _service_discoverer = std::make_shared<Discovery>(reg_host, base_service_name, put_cb, del_cb);
    }
    void make_server_object(int websocket_port, int http_port)
    {
        _websocket_port = websocket_port;
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
        GatewayServer::ptr server = std::make_shared<GatewayServer>(_websocket_port, 
                                                                _http_port, 
                                                                _redis_client,
                                                                _mm_channels,
                                                                _service_discoverer,
                                                                _speech_service_name,
                                                                _file_service_name,
                                                                _user_service_name,
                                                                _transmite_service_name,
                                                                _message_service_name,
                                                                _friend_service_name);
        return server;
    }
private:
    int _websocket_port;
    int _http_port;

    std::shared_ptr<sw::redis::Redis> _redis_client;

    std::string _speech_service_name;
    std::string _file_service_name;
    std::string _user_service_name;
    std::string _transmite_service_name;
    std::string _message_service_name;
    std::string _friend_service_name;
    ServiceManager::ptr _mm_channels;
    Discovery::ptr _service_discoverer;
};

}
