#include "presence_server.h"

DEFINE_bool(run_mode, false, "程序的运行模式 false-调试 ; true-发布");
DEFINE_string(log_file, "", "发布模式下，用于指定日志的输出文件");
DEFINE_int32(log_level, 0, "发布模式下，用于指定日志的输出等级");

DEFINE_int32(listen_port, 9050, "Presence 服务 RPC 监听端口");

DEFINE_string(registry_host, "http://127.0.0.1:2379", "服务注册中心地址");
DEFINE_string(base_service, "/service", "服务监控根目录");
DEFINE_string(push_service, "/service/push_service", "Push 子服务名称（用于服务发现）");
DEFINE_string(instance_name, "/presence_service/instance", "Presence 服务实例标识");

DEFINE_string(redis_host, "127.0.0.1", "Redis服务器访问地址");
DEFINE_int32(redis_port, 6379, "Redis服务器访问端口");
DEFINE_int32(redis_db, 0, "Redis默认库号");
DEFINE_bool(redis_keep_alive, true, "Redis长连接保活");

DEFINE_int32(change_scan_interval_sec, 5, "状态变化扫描间隔（秒）");

int main(int argc, char *argv[])
{
    google::ParseCommandLineFlags(&argc, &argv, true);
    chatnow::init_logger(FLAGS_run_mode, FLAGS_log_file, FLAGS_log_level);

    // Redis
    auto redis = std::make_shared<sw::redis::Redis>(
        fmt::format("tcp://{}:{}/{}", FLAGS_redis_host, FLAGS_redis_port, FLAGS_redis_db));

    // 服务发现
    auto channels = std::make_shared<chatnow::ServiceManager>();
    channels->declared(FLAGS_push_service);
    auto put_cb = std::bind(&chatnow::ServiceManager::onServiceOnline, channels.get(),
                            std::placeholders::_1, std::placeholders::_2);
    auto del_cb = std::bind(&chatnow::ServiceManager::onServiceOffline, channels.get(),
                            std::placeholders::_1, std::placeholders::_2);
    auto discovery = std::make_shared<chatnow::Discovery>(FLAGS_registry_host, FLAGS_base_service,
                                                           put_cb, del_cb);

    // Presence 服务实例
    auto impl = std::make_shared<chatnow::presence::PresenceServiceImpl>(
        redis, channels, FLAGS_push_service);

    // 启动变化扫描器
    impl->start_change_scanner(FLAGS_change_scan_interval_sec);

    // brpc server
    brpc::Server server;
    brpc::ServerOptions opt;
    opt.num_threads = 4;
    opt.idle_timeout_sec = 30;

    if (server.AddService(impl.get(), brpc::SERVER_OWNS_SERVICE) != 0) {
        LOG_ERROR("Presence 服务注册失败");
        return -1;
    }

    butil::EndPoint ep;
    if (butil::str2endpoint("0.0.0.0", FLAGS_listen_port, &ep) != 0) {
        LOG_ERROR("Presence 服务端口解析失败 port={}", FLAGS_listen_port);
        return -1;
    }

    if (server.Start(ep, &opt) != 0) {
        LOG_ERROR("Presence 服务启动失败");
        return -1;
    }

    // 注册到 etcd（3 层路径，与 getServiceName 兼容）
    auto reg = std::make_shared<chatnow::Registry>(FLAGS_registry_host);
    reg->registry(FLAGS_base_service + FLAGS_instance_name,
                  fmt::format("{}:{}", "0.0.0.0", FLAGS_listen_port));

    LOG_INFO("Presence 服务已启动 port={}", FLAGS_listen_port);

    server.RunUntilAskedToQuit();

    impl->stop_change_scanner();
    reg->unregister();
    server.Stop(0);
    server.Join();

    return 0;
}
