#include "speech_server.h"

DEFINE_bool(run_mode, false, "程序的运行模式 false-调试 ; true-发布");
DEFINE_string(log_file, "", "发布模式下，用于指定日志的输出文件");
DEFINE_int32(log_level, 0, "发布模式下，用于指定日志的输出等级");

DEFINE_string(registry_host, "http://127.0.0.1:2379", "服务注册中心地址");
DEFINE_string(base_service, "/service", "服务监控根目录");
DEFINE_string(instance_name, "/speech_service/instance", "服务监控根目录");
DEFINE_string(access_host, "127.0.0.1:10001", "当前实例的外部访问地址");

DEFINE_string(listen_port, 10001, "RPC服务器监听端口");
DEFINE_string(rpc_timeout, -1, "RPC调用超时时间");
DEFINE_string(rpc_threads, 1, "RPC的IO线程数量");


DEFINE_string(app_id, "121352894", "语言平台ID");
DEFINE_string(api_key, "ODUIu9elQJVph4CqUL6leN8T", "语言平台API密钥");
DEFINE_string(secret_key, "v6aLxw4m8k2ZnKWqr4lxmhmfNdmxMNP7", "语音平台加密密钥");

int main(int argc, char *argv[])
{
    google::ParseCommandLineFlags(&argc, &argv, true);
    chatnow::init_logger(FLAGS_run_mode, FLAGS_log_file, FLAGS_log_level);

    chatnow::SpeechServerBuilder ssb;
    ssb.make_asr_object(FLAGS_app_id, FLAGS_api_keym FLAGS_secret_key);
    ssb.make_rpc_object(FLAGS_listen_port, FLAGS_rpc_timeout, FLAGS_rpc_threads);
    ssb.make_reg_object(FLAGS_registry_host, FLAGS_base_service + FLAGS_instance_name, FLAGS_access_host);

    auto server = ssb.build();
    server->start();

    return 0;
}