#include "push_server.h"
#include "auth/jwt_codec.hpp"

DEFINE_bool(run_mode, false, "程序的运行模式 false-调试 ; true-发布");
DEFINE_string(log_file, "", "发布模式下，用于指定日志的输出文件");
DEFINE_int32(log_level, 0, "发布模式下，用于指定日志的输出等级");

DEFINE_string(registry_host, "http://127.0.0.1:2379", "服务注册中心地址");
DEFINE_string(base_service, "/service", "服务监控根目录");
DEFINE_string(instance_name, "/push_service/instance", "实例注册路径");
DEFINE_string(access_host, "127.0.0.1:10008", "当前实例对外可访问 brpc 地址");

DEFINE_int32(listen_port, 10008, "Push brpc 监听端口");
DEFINE_int32(ws_port, 9001, "Push WebSocket 监听端口");
DEFINE_int32(rpc_timeout, -1, "RPC调用超时时间");
DEFINE_int32(rpc_threads, 4, "RPC的IO线程数量");

DEFINE_string(message_service, "/service/message_service", "消息存储子服务名称（用于 ACK 收敛）");
DEFINE_string(push_service, "/service/push_service", "推送子服务名称（自身，便于跨实例转发）");

DEFINE_string(redis_host, "127.0.0.1", "Redis 服务器访问地址");
DEFINE_string(redis_seeds, "", "Redis Cluster 种子节点（逗号分隔，如 host1:6379,host2:6379）");
DEFINE_int32(redis_port, 6379, "Redis 端口");
DEFINE_int32(redis_db, 0, "Redis 库号");
DEFINE_bool(redis_keep_alive, true, "Redis 长连接");
DEFINE_int32(redis_pool_size, 16, "Redis 连接池大小");

DEFINE_string(mq_user, "root", "MQ 用户");
DEFINE_string(mq_pswd, "", "MQ password");
DEFINE_string(mq_host, "127.0.0.1:5672", "MQ 地址");
DEFINE_string(mq_push_exchange, "chat_push_exchange", "推送交换机");
DEFINE_string(mq_push_queue, "msg_push_queue", "推送队列");
DEFINE_string(mq_push_binding_key, "push", "推送绑定键");

// M5: 心跳触发未 ack 重传的可调参数
DEFINE_int32(resend_batch, 50, "心跳触发未 ack 重传的批量上限");
DEFINE_int32(resend_max_age_sec, 5, "未 ack 项入队后等待多少秒视为可重传");

// JWT — 统一从 auth.json 加载（与 identity/gateway 共享密钥源）
DEFINE_string(auth_config, "/im/conf/auth.json", "JWT 鉴权配置文件路径(JSON)");

int main(int argc, char *argv[])
{
    google::ParseCommandLineFlags(&argc, &argv, true);
    chatnow::init_logger(FLAGS_run_mode, FLAGS_log_file, FLAGS_log_level);

    chatnow::push::PushServerBuilder psb;
    psb.make_jwt_object(FLAGS_auth_config);

    psb.set_redis_seeds(FLAGS_redis_seeds);
    psb.make_redis_object(FLAGS_redis_host, FLAGS_redis_port, FLAGS_redis_db,
                          FLAGS_redis_keep_alive, FLAGS_redis_pool_size);
    psb.make_mq_object(FLAGS_mq_user, FLAGS_mq_pswd, FLAGS_mq_host,
                       FLAGS_mq_push_exchange, FLAGS_mq_push_queue, FLAGS_mq_push_binding_key);
    psb.make_discovery_object(FLAGS_registry_host, FLAGS_base_service, FLAGS_message_service, FLAGS_push_service);
    psb.make_reg_object(FLAGS_registry_host, FLAGS_base_service + FLAGS_instance_name, FLAGS_access_host);
    psb.set_resend_params(FLAGS_resend_batch, FLAGS_resend_max_age_sec);
    psb.set_etcd_client(std::make_shared<etcd::Client>(FLAGS_registry_host));
    psb.set_push_service_dir(FLAGS_base_service + FLAGS_push_service);
    psb.make_cross_reaper_election();
    psb.make_local_cache();
    psb.make_rpc_object(FLAGS_listen_port, FLAGS_rpc_timeout, FLAGS_rpc_threads, FLAGS_ws_port);

    auto server = psb.build();
    server->start();
    return 0;
}
