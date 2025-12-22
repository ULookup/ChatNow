#include "asr.hpp"      // 语言识别模块封装
#include "etcd.hpp"     // 服务注册模块封装
#include "logger.hpp"   // 日志模块封装
#include "speech.pb.h"  // protobuf框架代码
#include <brpc/server.h>    //
#include <butil/logging.h>  // 实现语音识别子服务

namespace chatnow
{

class SpeechServiceImpl : public SpeechService
{
public:
    SpeechServiceImpl(const ASRClient::ptr &asr_client) : _asr_client(asr_client) {}
    ~SpeechServiceImpl() = default;
    void SpeechRecognition(google::protobuf::RpcController *controller,
                        const ::chatnow::SpeechRecognitionReq *request,
                        ::chatnow::SpeechRecognitionRsp *response,
                        ::google::protobuf::Closure *done)
    {
        brpc::ClosureGuard rpc_guard(done);
        //1. 取出请求中的语言数据
        //2. 调用语音 SDK 模块进行语音识别，得到响应
        std::string err;
        std::string res = _asr_client->recognize(request->speech_content(), err);
        if(res.empty()) {
            LOG_ERROR("{} 语音识别失败!", request->request_id());
            response->set_request_id(request->request_id());
            response->set_success(false);
            response->set_errmsg("语音识别失败:" + err);
            return;
        }
        //3. 组织响应
        response->set_request_id(request->request_id());
        response->set_success(true);
        response->set_recognition_result(res);
    }
private:
    ASRClient::ptr _asr_client;
};

class SpeechServer
{
public:
    using ptr = std::shared_ptr<SpeechServer>;

    SpeechServer(const ASRClient::ptr &asr_client,const Registry::ptr &reg_client, const std::shared_ptr<brpc::Server> &server) 
        : _asr_client(asr_client), _reg_client(reg_client), _rpc_server(server) {}
    ~SpeechServer() = default;
    /* brief: 搭建RPC服务器，并启动服务器 */
    void start() {
        _rpc_server->RunUntilAskedToQuit();
    }
private:
    ASRClient::ptr _asr_client;
    Registry::ptr _reg_client;
    std::shared_ptr<brpc::Server> _rpc_server;
};

/* 建造者模式: 将对象真正的构造过程封装，便于后期扩展和调整 */
class SpeechServerBuilder
{
public:
    /* brief: 构造语音识别客户端对象 */
    void make_asr_object(const std::string &app_id,
                        const std::string &api_key,
                        const std::string &secret_key) {
        _asr_client = std::make_shared<ASRClient>(app_id, api_key, secret_key);
    }
    /* brief: 用于构造服务注册客户端对象 */
    void make_reg_object(const std::string &reg_host,
                        const std::string &service_name,
                        const std::string &access_host) {
        _reg_client = std::make_shared<Registry>(reg_host);
        _reg_client->registry(service_name, access_host);
    }
    /* brief: 构造RPC服务器对象，并添加服务 */
    void make_rpc_object(uint16_t port, uint32_t timeout, uint8_t num_threads) {
        if(!_asr_client) {
            LOG_ERROR("还未初始化语音识别模块");
            abort();
        }
        _rpc_server = std::make_shared<brpc::Server>();
        SpeechServiceImpl *speech_service = new SpeechServiceImpl(_asr_client);
        int ret = _rpc_server->AddService(speech_service, brpc::ServiceOwnership::SERVER_OWNS_SERVICE);
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
    SpeechServer::ptr build() {
        if(!_asr_client) {
            LOG_ERROR("还未初始化语音识别模块");
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
        SpeechServer::ptr server = std::make_shared<SpeechServer>(_asr_client, _reg_client, _rpc_server);
        return server;
    }
private:
    ASRClient::ptr _asr_client;
    Registry::ptr _reg_client;
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