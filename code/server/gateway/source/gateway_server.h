#pragma once

#include <brpc/server.h>
#include <butil/logging.h>

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
        _redis_status(std::make_shared<Status>(redis_client),
        _mm_channels(channels),
        _service_discoverer(service_discoverer),
        _speech_service_name(speech_service_name),
        _file_service_name(file_service_name),
        _user_service_name(user_service_name),
        _transmite_service_name(transmite_service_name),
        _message_service_name(message_service_name),
        _friend_service_name(friend_service_name)) 
    {
        //1. 搭建websocket服务器
        _ws_server.set_access_channels(websocketpp::log::alevel::none);
        _ws_server.init_asio();
        _ws_server.set_open_handler(&GatewayServer::onOpen);
        _ws_server.set_close_handler(&GatewayServer::onClose);
        auto ws_callback = std::bind(&GatewayServer::OnMessage, this, std::placeholders::_1, std::placeholders::_2);
        _ws_server.set_message_handler(ws_callback);
        _ws_server.set_reuse_addr(true);
        _ws_server.listen(websocket_port);
        _ws_server.start_accept();

        //2. 搭建http服务器
        _http_server.Post(GET_MAIL_VERIFY_CODE,         std::bind(&GatewayServer::GetMailVerifyCode,         this, std::placeholders::_1, std::placeholders::_2));
        _http_server.Post(USERNAME_REGISTER,            std::bind(&GatewayServer::UserRegister,              this, std::placeholders::_1, std::placeholders::_2));
        _http_server.Post(USERNAME_LOGIN,               std::bind(&GatewayServer::UserLogin,                 this, std::placeholders::_1, std::placeholders::_2));
        _http_server.Post(MAIL_REGISTER,                std::bind(&GatewayServer::MailRegister,              this, std::placeholders::_1, std::placeholders::_2));
        _http_server.Post(MAIL_LOGIN,                   std::bind(&GatewayServer::MailLogin,                 this, std::placeholders::_1, std::placeholders::_2));
        _http_server.Post(GET_USER_INFO,                std::bind(&GatewayServer::GetUserInfo,               this, std::placeholders::_1, std::placeholders::_2));
        _http_server.Post(SET_AVATAR,                   std::bind(&GatewayServer::SetUserAvatar,             this, std::placeholders::_1, std::placeholders::_2));
        _http_server.Post(SET_NICKNAME,                 std::bind(&GatewayServer::SetUserNickname,           this, std::placeholders::_1, std::placeholders::_2));
        _http_server.Post(SET_DESCRIPTION,              std::bind(&GatewayServer::SetUserDescription,        this, std::placeholders::_1, std::placeholders::_2));
        _http_server.Post(SET_MAIL,                     std::bind(&GatewayServer::SetUserMailNumber,         this, std::placeholders::_1, std::placeholders::_2));
        _http_server.Post(GET_FRIEND_LIST,              std::bind(&GatewayServer::GetFriendList,             this, std::placeholders::_1, std::placeholders::_2));
        _http_server.Post(ADD_FRIEND_APPLY,             std::bind(&GatewayServer::FriendAdd,                 this, std::placeholders::_1, std::placeholders::_2));
        _http_server.Post(ADD_FRIEND_PROCESS,           std::bind(&GatewayServer::FriendAddProcess,          this, std::placeholders::_1, std::placeholders::_2));
        _http_server.Post(REMOVE_FRIEND,                std::bind(&GatewayServer::FriendRemove,              this, std::placeholders::_1, std::placeholders::_2));
        _http_server.Post(SEARCH_FRIEND,                std::bind(&GatewayServer::FriendSearch,              this, std::placeholders::_1, std::placeholders::_2));
        _http_server.Post(GET_CHAT_SESSION_LIST,        std::bind(&GatewayServer::GetChatSessionList,        this, std::placeholders::_1, std::placeholders::_2));
        _http_server.Post(CREATE_CHAT_SESSION,          std::bind(&GatewayServer::ChatSessionCreate,         this, std::placeholders::_1, std::placeholders::_2));
        _http_server.Post(GET_CHAT_SESSION_MEMBER,      std::bind(&GatewayServer::GetChatSessionMember,      this, std::placeholders::_1, std::placeholders::_2));
        _http_server.Post(GET_PENDING_FRIEND_EVENTS,    std::bind(&GatewayServer::GetPendingFriendEventList, this, std::placeholders::_1, std::placeholders::_2));
        _http_server.Post(GET_HISTORY,                  std::bind(&GatewayServer::GetHistoryMsg,             this, std::placeholders::_1, std::placeholders::_2));
        _http_server.Post(GET_RECENT,                   std::bind(&GatewayServer::GetRecentMsg,              this, std::placeholders::_1, std::placeholders::_2));
        _http_server.Post(SEARCH_HISTORY,               std::bind(&GatewayServer::MsgSearch,                 this, std::placeholders::_1, std::placeholders::_2));
        _http_server.Post(NEW_MESSAGE,                  std::bind(&GatewayServer::GetTransmitTarget,         this, std::placeholders::_1, std::placeholders::_2));
        _http_server.Post(GET_SINGLE_FILE,              std::bind(&GatewayServer::GetSingleFile,             this, std::placeholders::_1, std::placeholders::_2));
        _http_server.Post(GET_MULTI_FILE,               std::bind(&GatewayServer::GetMultiFile,              this, std::placeholders::_1, std::placeholders::_2));
        _http_server.Post(PUT_SINGLE_FILE,              std::bind(&GatewayServer::PutSingleFile,             this, std::placeholders::_1, std::placeholders::_2));
        _http_server.Post(PUT_MULTI_FILE,               std::bind(&GatewayServer::PutMultiFile,              this, std::placeholders::_1, std::placeholders::_2));
        _http_server.Post(RECOGNITION,                  std::bind(&GatewayServer::SpeechRecognition,         this, std::placeholders::_1, std::placeholders::_2));

        _http_thread = std::thread([this, http_port](){
            _http_server.listen("0.0.0.0", http_port);
        })
        _http_thread.detach();
    }
    void start() {
        _ws_server.run();
    }
private:
    void onOpen(websocketpp::connection_hdl hd1) { LOG_DEBUG("WebSocket 长连接建立成功 {}", _ws_server.get_con_from_hdl(hdl).get()); }

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
        LOG_DEBUG("{} - {} - {} 长连接断开，清理缓存数据", conn->get(), uid, ssid);
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
        std::string ssid = request->session_id();
        _redis_session->uid(ssid);
        if(!uid) {
            //4. 会话信息不存在则关闭连接
            LOG_ERROR("长连接身份识别失败：未找到会话信息 {}", ssid);
            _ws_server.close(hdl, websocketpp::close::status::unsupported_data, "未找到会话信息");
            return;
        }
        //5. 会话信息存在，则添加长连接管理
        _connections->insert(conn, *uid, ssid);
        LOG_DEBUG("新增长连接管理: {} - {} - {}", conn.get(), *uid, ssid);
    }

    void GetMailVerifyCode(const httplib::Request &request, httplib::Response &response) {
        //1. 取出http请求正文，将正文进行反序列化
        MailVerifyCodeReq req;
        MailVerifyCodeRsp rsp;
        auto err_response = [&rso, &rsp, &response](const std::string &errmsg) -> void {
            rsp.set_success(false);
            rsp.set_errmsg(errmsg);
            response.set_contetn(rsp.SerializeAsString(), "application/x-protobuf");
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
        stub.GetMailVerifyCode(&cntl, &req, &rsp, nullptr);
        if(cntl.Failed()) {
            LOG_ERROR("请求ID - {} 用户子服务调用失败: {}", req.request_id(), cntl.Errortext());
            return err_response("用户子服务调用失败");
        }
        //3. 得到用户子服务的响应后，将响应进行序列化作为http响应正文
        response.set_content(rsp.SerializeAsString(), "application/x-protobuf");
    }

    void UserRegister(const httplib::Request &request, httplib::Response &response) {
        //1. 取出http请求正文，将正文进行反序列化
        UserRegisterReq req;
        UserRegisterRsp rsp;
        auto err_response = [&rso, &rsp, &response](const std::string &errmsg) -> void {
            rsp.set_success(false);
            rsp.set_errmsg(errmsg);
            response.set_contetn(rsp.SerializeAsString(), "application/x-protobuf");
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
        stub.UserRegister(&cntl, &req, &rsp, nullptr);
        if(cntl.Failed()) {
            LOG_ERROR("请求ID - {} 用户子服务调用失败: {}", req.request_id(), cntl.Errortext());
            return err_response("用户子服务调用失败");
        }
        //3. 得到用户子服务的响应后，将响应进行序列化作为http响应正文
        response.set_content(rsp.SerializeAsString(), "application/x-protobuf");
    }
    void UserLogin(const httplib::Request &request, httplib::Response &response) {
        //1. 取出http请求正文，将正文进行反序列化
        UserLoginReq req;
        UserLoginRsp rsp;
        auto err_response = [&rso, &rsp, &response](const std::string &errmsg) -> void {
            rsp.set_success(false);
            rsp.set_errmsg(errmsg);
            response.set_contetn(rsp.SerializeAsString(), "application/x-protobuf");
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
        stub.UserLogin(&cntl, &req, &rsp, nullptr);
        if(cntl.Failed()) {
            LOG_ERROR("请求ID - {} 用户子服务调用失败: {}", req.request_id(), cntl.Errortext());
            return err_response("用户子服务调用失败");
        }
        //3. 得到用户子服务的响应后，将响应进行序列化作为http响应正文
        response.set_content(rsp.SerializeAsString(), "application/x-protobuf");
    }
    void MailRegister(const httplib::Request &request, httplib::Response &response) {
        //1. 取出http请求正文，将正文进行反序列化
        MailRegisterReq req;
        MailRegisterRsp rsp;
        auto err_response = [&rso, &rsp, &response](const std::string &errmsg) -> void {
            rsp.set_success(false);
            rsp.set_errmsg(errmsg);
            response.set_contetn(rsp.SerializeAsString(), "application/x-protobuf");
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
        stub.MailRegister(&cntl, &req, &rsp, nullptr);
        if(cntl.Failed()) {
            LOG_ERROR("请求ID - {} 用户子服务调用失败: {}", req.request_id(), cntl.Errortext());
            return err_response("用户子服务调用失败");
        }
        //3. 得到用户子服务的响应后，将响应进行序列化作为http响应正文
        response.set_content(rsp.SerializeAsString(), "application/x-protobuf");
    }
    void MailLogin(const httplib::Request &request, httplib::Response &response) {
        //1. 取出http请求正文，将正文进行反序列化
        MailLoginReq req;
        MailLoginRsp rsp;
        auto err_response = [&rso, &rsp, &response](const std::string &errmsg) -> void {
            rsp.set_success(false);
            rsp.set_errmsg(errmsg);
            response.set_contetn(rsp.SerializeAsString(), "application/x-protobuf");
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
        stub.MailLogin(&cntl, &req, &rsp, nullptr);
        if(cntl.Failed()) {
            LOG_ERROR("请求ID - {} 用户子服务调用失败: {}", req.request_id(), cntl.Errortext());
            return err_response("用户子服务调用失败");
        }
        //3. 得到用户子服务的响应后，将响应进行序列化作为http响应正文
        response.set_content(rsp.SerializeAsString(), "application/x-protobuf");
    }
    void GetUserInfo(const httplib::Request &request, httplib::Response &response);
    void SetUserAvatar(const httplib::Request &request, httplib::Response &response);
    void SetUserNickname(const httplib::Request &request, httplib::Response &response);
    void SetUserDescription(const httplib::Request &request, httplib::Response &response);
    void SetUserMailNumber(const httplib::Request &request, httplib::Response &response);
    void GetFriendList(const httplib::Request &request, httplib::Response &response);
    void FriendAdd(const httplib::Request &request, httplib::Response &response);
    void FriendAddProcess(const httplib::Request &request, httplib::Response &response);
    void FriendRemove(const httplib::Request &request, httplib::Response &response);
    void FriendSearch(const httplib::Request &request, httplib::Response &response);
    void GetChatSessionList(const httplib::Request &request, httplib::Response &response);
    void ChatSessionCreate(const httplib::Request &request, httplib::Response &response);
    void GetChatSessionMember(const httplib::Request &request, httplib::Response &response);
    void GetPendingFriendEventList(const httplib::Request &request, httplib::Response &response);
    void GetHistoryMsg(const httplib::Request &request, httplib::Response &response);
    void GetRecentMsg(const httplib::Request &request, httplib::Response &response);
    void MsgSearch(const httplib::Request &request, httplib::Response &response);
    void GetTransmitTarget(const httplib::Request &request, httplib::Response &response);
    void GetSingleFile(const httplib::Request &request, httplib::Response &response);
    void GetMultiFile(const httplib::Request &request, httplib::Response &response);
    void PutSingleFile(const httplib::Request &request, httplib::Response &response);
    void PutMultiFile(const httplib::Request &request, httplib::Response &response);
    void SpeechRecognition(const httplib::Request &request, httplib::Response &response);
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

        _service_discover = std::make_shared<Discovery>(reg_host, base_service_name, put_cb, del_cb);
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
        if(!_service_discover) {
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
}

}
