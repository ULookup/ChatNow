#pragma once

#include "etcd.hpp"     // 服务注册模块封装
#include "logger.hpp"   // 日志模块封装
#include "rabbitmq.hpp"
#include "channel.hpp"
#include "utils.hpp"
#include "snowflake.hpp"
#include "base.pb.h"
#include "user.pb.h"
#include "chatsession.pb.h"
#include "transmite.pb.h"  // protobuf框架代码
#include <brpc/server.h>    //
#include <butil/logging.h>  // 实现语音识别子服务

namespace chatnow
{

class TransmiteServiceImpl : public chatnow::MsgTransmitService
{
public:
    TransmiteServiceImpl(const std::string &user_service_name,
                        const std::string &chatsession_service_name, 
                        const ServiceManager::ptr &channels, 
                        const std::string &exchange_name,
                        const std::string &routing_key,
                        const MQClient::ptr &mq_client,
                        const std::shared_ptr<SnowflakeId> &id_generator) 
                        : _user_service_name(user_service_name),
                        _chatsession_service_name(chatsession_service_name),
                        _mm_channels(channels),
                        _exchange_name(exchange_name),
                        _routing_key(routing_key),
                        _mq_client(mq_client),
                        _id_generator(id_generator) {}
    ~TransmiteServiceImpl() = default;
    void GetTransmitTarget(google::protobuf::RpcController *controller,
                        const ::chatnow::NewMessageReq *request,
                        ::chatnow::GetTransmitTargetRsp *response,
                        ::google::protobuf::Closure *done)
    {
        brpc::ClosureGuard rpc_guard(done);
        auto err_response = [this, response](const std::string &rid, const std::string &err_msg) -> void {
            response->set_request_id(rid);
            response->set_success(false);
            response->set_errmsg(err_msg);
            return;
        };        
        // 从请求中获取：用户ID，所属会话ID，消息内容
        std::string rid = request->request_id();
        std::string uid = request->user_id();
        std::string chat_ssid = request->chat_session_id();
        const MessageContent &content = request->message();
        // 进行消息组织: 发送者-用户子服务获取，所属会话，消息内容，产生时间，消息ID
        // 1.获取会话用户信息
        auto channel = _mm_channels->choose(_user_service_name);
        if(!channel) {
            LOG_ERROR("请求ID: {} - {} 没有可供访问的用户子服务节点", rid, _user_service_name);
            return err_response(rid, "没有可供访问的用户子服务节点");
        }
        UserService_Stub stub(channel.get());
        GetUserInfoReq req;
        GetUserInfoRsp rsp;
        req.set_request_id(rid);
        req.set_user_id(uid);
        brpc::Controller cntl;
        stub.GetUserInfo(&cntl, &req, &rsp, nullptr);
        if(cntl.Failed() == true || rsp.success() == false) {
            LOG_ERROR("请求ID: {} - 用户子服务调用失败: {}", rid, cntl.ErrorText());
            return err_response(rid, "用户子服务调用失败");
        }
        // 2.组织消息
        MessageInfo message;
        message.set_message_id(_id_generator->Next());
        message.set_chat_session_id(chat_ssid);
        message.set_timestamp(time(nullptr));
        message.mutable_sender()->CopyFrom(rsp.user_info());
        message.mutable_message()->CopyFrom(content);
        // 2.1 获取消息转发客户端用户列表
        std::vector<std::string> member_id_list;
        bool ret = _GetMembers(chat_ssid, member_id_list);
        if(ret == false) {
            LOG_ERROR("请求ID - {} 获取会话成员ID列表失败");
            return err_response(rid, "获取成员ID列表失败");
        }
        // 将封装完毕的消息，发布到消息队列，待消息存储子服务进行消息持久化
        ret = _mq_client->publish(_exchange_name, message.SerializeAsString(), _routing_key);
        if(ret == false) {
            LOG_ERROR("请求ID: {} - 持久化消息发布失败", rid);
            return err_response(rid, "持久化消息发布失败");
        }
        // 组织响应
        response->set_request_id(rid);
        response->set_success(true);
        response->mutable_message()->CopyFrom(message);
        for(const auto &id : member_id_list) {
            response->add_target_id_list(id);
        }
    }
private:
    bool _GetMembers(const std::string &ssid, std::vector<std::string> &member_id_list) {
        auto channel = _mm_channels->choose(_chatsession_service_name);
        if(!channel) {
            LOG_ERROR("{} 没有可供访问的会话管理子服务节点", _chatsession_service_name);
            return false;
        }
        ChatSessionService_Stub stub(channel.get());
        GetMemberIdListReq req;
        GetMemberIdListRsp rsp;
        req.set_chat_session_id(ssid);
        brpc::Controller cntl;
        stub.GetMemberIdList(&cntl, &req, &rsp, nullptr);
        if(cntl.Failed() == true || rsp.success() == false) {
            LOG_ERROR("会话子服务调用失败: {}", cntl.ErrorText());
            return false;
        }
        for(int i = 0; i < rsp.member_id_list_size(); ++i) {
            member_id_list.push_back(rsp.member_id_list(i));
        }
        return true;
    }
private:
    std::string _user_service_name;
    std::string _chatsession_service_name;
    ServiceManager::ptr _mm_channels;

    // 消息队列客户端句柄
    std::string _exchange_name;
    std::string _routing_key;
    MQClient::ptr _mq_client;
    std::shared_ptr<SnowflakeId> _id_generator;
};

class TransmiteServer
{
public:
    using ptr = std::shared_ptr<TransmiteServer>;

    TransmiteServer(const Discovery::ptr &service_discover, 
                const Registry::ptr &reg_client, 
                const std::shared_ptr<brpc::Server> &server) 
        : _service_discover(service_discover), _reg_client(reg_client), _rpc_server(server) {}
    ~TransmiteServer() = default;
    /* brief: 搭建RPC服务器，并启动服务器 */
    void start() {
        _rpc_server->RunUntilAskedToQuit();
    }
private:
    Discovery::ptr _service_discover;   // 服务发现客户端
    Registry::ptr _reg_client;          // 服务注册客户端
    std::shared_ptr<brpc::Server> _rpc_server;
};

/* 建造者模式: 将对象真正的构造过程封装，便于后期扩展和调整 */
class TransmiteServerBuilder
{
public:
    /* brief: 构造分布式有序ID生成器 */
    void make_id_generator_object(uint64_t worker_id, uint64_t epoch_ms, bool wait_on_clock_backwards)
    {
        try {
            _id_generator = std::make_shared<SnowflakeId>(worker_id, epoch_ms, wait_on_clock_backwards);
        } catch(std::exception &e) {
            LOG_ERROR("构造分布式有序ID生成器失败: {}", e.what());
        }
    }
    /* brief: 用于构造服务发现&信道管理客户端对象 */
    void make_discovery_object(const std::string &reg_host, 
                            const std::string &base_service_name,
                            const std::string &user_service_name,
                            const std::string &chatsession_service_name)
    {
        _user_service_name = user_service_name;
        _chatsession_service_name = chatsession_service_name;
        _mm_channels = std::make_shared<ServiceManager>();
        _mm_channels->declared(_user_service_name);
        _mm_channels->declared(_chatsession_service_name);
        auto put_cb = std::bind(&ServiceManager::onServiceOnline, _mm_channels.get(), std::placeholders::_1, std::placeholders::_2);
        auto del_cb = std::bind(&ServiceManager::onServiceOffline, _mm_channels.get(), std::placeholders::_1, std::placeholders::_2);

        _service_discover = std::make_shared<Discovery>(reg_host, base_service_name, put_cb, del_cb);
    }
    /* brief: 用于构造服务注册客户端对象 */
    void make_reg_object(const std::string &reg_host,
                        const std::string &service_name,
                        const std::string &access_host) {
        _reg_client = std::make_shared<Registry>(reg_host);
        _reg_client->registry(service_name, access_host);
    }
    /* brief: 构造 RabbitMQ 客户端对象 */
    void make_mq_object(const std::string &user, 
                    const std::string &password, 
                    const std::string &host,
                    const std::string &exchange_name,
                    const std::string &queue_name,
                    const std::string &binding_key)
    {
        _exchange_name = exchange_name;
        _routing_key = binding_key;
        _mq_client = std::make_shared<MQClient>(user, password, host); 
        _mq_client->declareComponents(exchange_name, queue_name, binding_key);
    }
    /* brief: 构造RPC服务器对象，并添加服务 */
    void make_rpc_object(uint16_t port, uint32_t timeout, uint8_t num_threads) {
        if(!_id_generator) {
            LOG_ERROR("还未初始化分布式ID生成器");
            abort();
        }
        if(!_mq_client) {
            LOG_ERROR("还未初始化消息队列客户端模块");
            abort();
        }
        if(!_mm_channels) {
            LOG_ERROR("还未初始化信道管理模块");
            abort();
        }
        _rpc_server = std::make_shared<brpc::Server>();
        TransmiteServiceImpl *transmite_service = new TransmiteServiceImpl(_user_service_name, 
                                                                        _chatsession_service_name,
                                                                        _mm_channels, 
                                                                        _exchange_name, 
                                                                        _routing_key, 
                                                                        _mq_client,
                                                                        _id_generator);
        int ret = _rpc_server->AddService(transmite_service, brpc::ServiceOwnership::SERVER_OWNS_SERVICE);
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
    TransmiteServer::ptr build() {
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

        TransmiteServer::ptr server = std::make_shared<TransmiteServer>(_service_discover, _reg_client, _rpc_server);
        return server;
    }
private:
    std::string _user_service_name;
    std::string _chatsession_service_name;
    ServiceManager::ptr _mm_channels;

    std::string _exchange_name;
    std::string _routing_key;
    MQClient::ptr _mq_client;
    std::shared_ptr<SnowflakeId> _id_generator;
    
    Discovery::ptr _service_discover;   // 服务发现客户端
    Registry::ptr _reg_client;          // 服务注册客户端
    std::shared_ptr<brpc::Server> _rpc_server;


};

}

///* brief: 请求 */
//message SpeechRecognitionReq { 
//    string request_id = 1;          // 请求ID
//    bytes speech_content = 2;       // 语言数据
//   optional string user_id = 3;    //用户ID (option 表示可选项)
//   optional string session_id = 4; //登录会话ID -- 网关进行身份鉴权
//} 
///* brief: 响应 */
//message SpeechRecognitionRsp { 
//    string request_id = 1;          // 请求ID
//    bool success = 2;               // 请求处理结果标志
//    optional string errmsg = 3;     // 失败原因
//    string recognition_result = 4;  // 识别后的文字数据
//} 
//// brief: 语音识别 Rpc 服务及接口定义
//service SpeechService { 
//    rpc SpeechRecognition(SpeechRecognitionReq) returns (SpeechRecognitionRsp); 
//}