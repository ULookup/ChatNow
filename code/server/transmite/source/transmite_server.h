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
                        const Publisher::ptr &publisher,
                        const std::shared_ptr<SnowflakeId> &id_generator) 
                        : _user_service_name(user_service_name),
                        _chatsession_service_name(chatsession_service_name),
                        _mm_channels(channels),
                        _exchange_name(exchange_name),
                        _routing_key(routing_key),
                        _publisher(publisher),
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
        // 1. 获取 Channel
        //LOG_DEBUG("请求ID: {} - 获取服务通信信道", rid);
        auto user_channel = _mm_channels->choose(_user_service_name);
        auto session_channel = _mm_channels->choose(_chatsession_service_name);
        if (!user_channel || !session_channel) {
            LOG_ERROR("请求ID: {} - 服务节点缺失 (User or Session)", rid);
            return err_response(rid, "依赖服务节点缺失");
        }
        // 2. 准备并行 RPC 请求
        // --- 准备用户服务请求 ---
        //LOG_DEBUG("请求ID: {} - 准备用户服务 RPC 请求", rid);
        UserService_Stub user_stub(user_channel.get());
        GetUserInfoReq user_req;
        GetUserInfoRsp user_rsp;
        brpc::Controller user_cntl;
        
        user_req.set_request_id(rid);
        user_req.set_user_id(uid);

        // --- 准备会话服务请求 ---
        //LOG_DEBUG("请求ID: {} - 准备会话服务 RPC 请求", rid);
        ChatSessionService_Stub session_stub(session_channel.get());
        GetMemberIdListReq session_req;
        GetMemberIdListRsp session_rsp;
        brpc::Controller session_cntl;

        session_req.set_request_id(rid);
        session_req.set_chat_session_id(chat_ssid);

        // 3. 发起异步调用 (RPC 前置 & 并行化)
        //LOG_DEBUG("请求ID: {} - 发起并行 RPC 调用", rid);
        // 关键点：使用 brpc::DoNothing() 让调用立即返回，不阻塞当前线程
        user_stub.GetUserInfo(&user_cntl, &user_req, &user_rsp, brpc::DoNothing());
        session_stub.GetMemberIdList(&session_cntl, &session_req, &session_rsp, brpc::DoNothing());

        // 4. 等待所有 RPC 完成 (Join)
        //LOG_DEBUG("请求ID: {} - 等待 RPC 调用完成", rid);
        // bthread 会挂起当前协程，让出 CPU，直到两个 RPC 都回来
        brpc::Join(user_cntl.call_id());
        brpc::Join(session_cntl.call_id());

        // 5. 检查结果
        //LOG_DEBUG("请求ID: {} - 检查 RPC 调用结果", rid);
        if (user_cntl.Failed() || !user_rsp.success()) {
            LOG_ERROR("请求ID: {} - 获取用户信息失败: {}", rid, user_cntl.ErrorText());
            return err_response(rid, "获取用户信息失败");
        }
        if (session_cntl.Failed() || !session_rsp.success()) {
            LOG_ERROR("请求ID: {} - 获取群成员失败: {}", rid, session_cntl.ErrorText());
            return err_response(rid, "获取群成员失败");
        }
        // 6. 组装胖消息 (InternalMessage) 并发 MQ
        //LOG_DEBUG("请求ID: {} - 组装内部消息并发 MQ", rid);
        InternalMessage internal_msg; // 使用我们在 proto 里新定义的结构
        MessageInfo* msg_info = internal_msg.mutable_message_info();

        // 填充基础信息
        msg_info->set_message_id(_id_generator->Next());
        msg_info->set_chat_session_id(chat_ssid);
        msg_info->set_timestamp(time(nullptr));
        msg_info->mutable_message()->CopyFrom(request->message());
        
        // 填充发送者信息 (来自并行 RPC 的结果)
        msg_info->mutable_sender()->CopyFrom(user_rsp.user_info());

        // 填充群成员列表 (来自并行 RPC 的结果) -> 实现写扩散的关键
        for (const auto& member_id : session_rsp.member_id_list()) {
            internal_msg.add_member_id_list(member_id);
        }
        // 7. 组装响应给客户端的结果
        //LOG_DEBUG("请求ID: {} - 组装 RPC 响应", rid);
        response->set_request_id(rid);
        response->mutable_message()->CopyFrom(*msg_info);
        for(const auto& member_id : session_rsp.member_id_list()) {
            response->add_target_id_list(member_id);
        }

        // 解除rpc_guard对done管理权，这样在当前函数结束时，不会自动调用done->Run()，返回给客户端的响应由我们自己控制，可以在异步MQ发送完成后再调用done->Run()
        google::protobuf::Closure* async_done = rpc_guard.release();

        // 8. 发 MQ
        //LOG_DEBUG("请求ID: {} - 发 MQ", rid);
        _publisher->publish_confirm(internal_msg.SerializeAsString(), [async_done, response, rid](PublishStatus status, const std::string& msg) {
            if(status == PublishStatus::Acked) {
                LOG_DEBUG("请求ID: {} - 消息成功已成功投递到 Broker", rid);
                response->set_success(true);
            } else {
                LOG_DEBUG("请求ID: {} - 消息投递到 Broker 失败: {}", rid, msg);
                response->set_success(false);
                response->clear_message();        // 如果MQ投递失败，响应里就不返回消息内容了，避免客户端误以为消息已经投递成功了
                response->clear_target_id_list(); // 同理，目标列表也清空，客户端就不会去投递了
                response->set_errmsg("消息投递失败,请重试");
            }

            async_done->Run(); // 无论成功与否，MQ发送完成后都要调用done->Run()，让RPC响应返回给客户端
        });
    }
private:
    std::string _user_service_name;
    std::string _chatsession_service_name;
    ServiceManager::ptr _mm_channels;

    // 消息队列客户端句柄
    std::string _exchange_name;
    std::string _routing_key;
    Publisher::ptr _publisher;
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
        std::string amqp_url = "amqp://" + user + ":" + password + "@" + host + ":5672/";
        _mq_client = std::make_shared<MQClient>(amqp_url);
        declare_settings settings {
            .exchange = exchange_name,
            .exchange_type = chatnow::FANOUT,
            .queue = queue_name,
            .binding_key = binding_key
        };
        _publisher = std::make_shared<Publisher>(_mq_client, settings);
    }
    /* brief: 构造RPC服务器对象，并添加服务 */
    void make_rpc_object(uint16_t port, uint32_t timeout, uint8_t num_threads) {
        if(!_id_generator) {
            LOG_ERROR("还未初始化分布式ID生成器");
            abort();
        }
        if(!_publisher || !_mq_client) {
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
                                                                        _publisher,
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
    Publisher::ptr _publisher;
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