#include "transmite_server.h"

DEFINE_bool(run_mode, false, "程序的运行模式 false-调试 ; true-发布");
DEFINE_string(log_file, "", "发布模式下，用于指定日志的输出文件");
DEFINE_int32(log_level, 0, "发布模式下，用于指定日志的输出等级");

DEFINE_string(registry_host, "http://127.0.0.1:2379", "服务注册中心地址");
DEFINE_string(base_service, "/service", "服务监控根目录");
DEFINE_string(instance_name, "/transmite_service/instance", "服务监控根目录");
DEFINE_string(access_host, "127.0.0.1:10004", "当前实例的外部访问地址");
DEFINE_int32(instance_num, 1, "实例编号");

DEFINE_uint64(epoch_ms, 1735689600000ULL, "2025-01-01 UTC");
DEFINE_bool(wait_on_clock_backwards, true, "时钟回拨开关");

DEFINE_int32(listen_port, 10004, "RPC服务器监听端口");
DEFINE_int32(rpc_timeout, -1, "RPC调用超时时间");
DEFINE_int32(rpc_threads, 4, "RPC的IO线程数量");

DEFINE_string(identity_service, "/service/identity_service", "用户管理子服务名称");
DEFINE_string(conversation_service, "/service/conversation_service", "会话管理子服务名称");
DEFINE_string(message_service, "/service/message_service", "消息存储子服务名称（用于幂等查询）");

DEFINE_string(redis_host, "127.0.0.1", "Redis 服务器访问地址");
DEFINE_string(redis_seeds, "", "Redis Cluster 种子节点（逗号分隔，如 host1:6379,host2:6379）");
DEFINE_int32(redis_port, 6379, "Redis 服务器访问端口");
DEFINE_int32(redis_db, 0, "Redis 选择的库");
DEFINE_bool(redis_keep_alive, true, "Redis 长连接");
DEFINE_int32(redis_pool_size, 8, "Redis 连接池大小");

DEFINE_string(mq_user, "root", "消息队列服务器访问用户名");
DEFINE_string(mq_pswd, "", "消息队列服务器访问密码（可通过 CHATNOW_MQ_PSWD 环境变量设置）");
DEFINE_string(mq_host, "127.0.0.1:5672", "消息队列服务器访问地址");
// publisher-only：exchange 必须与 message 服务 mq_msg_exchange 一致
DEFINE_string(mq_msg_exchange, "chat_msg_exchange", "持久化消息的发布交换机名称（FANOUT，必须与 message.mq_msg_exchange 完全一致）");
DEFINE_string(mq_msg_queue, "", "publisher-only：留空，避免声明孤儿队列");
DEFINE_string(mq_msg_binding_key, "", "publisher-only：留空");

DEFINE_int32(rate_limit_user_max, 600, "用户每分钟最大消息数");
DEFINE_int32(rate_limit_session_max, 3000, "会话每分钟最大消息数");
DEFINE_int32(rate_limit_window_sec, 60, "限流窗口秒数");



int main(int argc, char *argv[])
{
    google::ParseCommandLineFlags(&argc, &argv, true);
    chatnow::init_logger(FLAGS_run_mode, FLAGS_log_file, FLAGS_log_level);

    // 环境变量兜底：配置文件中不应含密码
    if (FLAGS_mq_pswd.empty()) {
        const char *env = std::getenv("CHATNOW_MQ_PSWD");
        if (env && env[0] != '\0') FLAGS_mq_pswd = env;
    }
    if (FLAGS_mq_pswd.empty()) {
        LOG_ERROR("MQ 密码未设置（请通过 -mq_pswd 或 CHATNOW_MQ_PSWD 环境变量提供）");
        return 1;
    }

    chatnow::TransmiteServerBuilder tsb;
    // 注意：先初始化 Redis（worker_id 自动分配依赖 Redis），再初始化 ID 生成器
    tsb.set_redis_seeds(FLAGS_redis_seeds);
    tsb.make_redis_object(FLAGS_redis_host, FLAGS_redis_port, FLAGS_redis_db, FLAGS_redis_keep_alive, FLAGS_redis_pool_size);
    tsb.set_instance_owner(FLAGS_access_host);
    tsb.set_etcd_client(std::make_shared<etcd::Client>(FLAGS_registry_host));
    tsb.make_local_cache();
    tsb.make_id_generator_object(FLAGS_instance_num, FLAGS_epoch_ms, FLAGS_wait_on_clock_backwards);
    tsb.make_mq_object(FLAGS_mq_user, FLAGS_mq_pswd, FLAGS_mq_host, FLAGS_mq_msg_exchange, FLAGS_mq_msg_queue, FLAGS_mq_msg_binding_key);
    tsb.make_discovery_object(FLAGS_registry_host, FLAGS_base_service, FLAGS_identity_service, FLAGS_conversation_service, FLAGS_message_service);
    tsb.make_rpc_object(FLAGS_listen_port, FLAGS_rpc_timeout, FLAGS_rpc_threads);
    tsb.make_reg_object(FLAGS_registry_host, FLAGS_base_service + FLAGS_instance_name, FLAGS_access_host);

    auto server = tsb.build();
    server->start();

    return 0;
}