#include "conversation_server.h"

DEFINE_bool(run_mode, false, "程序的运行模式 false-调试 ; true-发布");
DEFINE_string(log_file, "", "发布模式下，用于指定日志的输出文件");
DEFINE_int32(log_level, 0, "发布模式下，用于指定日志的输出等级");

DEFINE_string(registry_host, "http://127.0.0.1:2379", "服务注册中心地址");
DEFINE_string(base_service, "/service", "服务监控根目录");
DEFINE_string(instance_name, "/conversation_service/instance", "服务实例 etcd 路径");
DEFINE_string(access_host, "127.0.0.1:10007", "当前实例的外部访问地址");

DEFINE_int32(listen_port, 10007, "RPC服务器监听端口");
DEFINE_int32(rpc_timeout, -1, "RPC调用超时时间");
DEFINE_int32(rpc_threads, 1, "RPC的IO线程数量");

DEFINE_string(identity_service, "/service/identity_service", "Identity 子服务名（GetMultiUserInfo）");
DEFINE_string(media_service,    "/service/media_service",    "Media 子服务名（保留）");
DEFINE_string(message_service,  "/service/message_service",  "Message 子服务名（SyncMessages 取 last_message）");

DEFINE_string(es_host, "http://127.0.0.1:9200/", "ES搜索引擎服务器URL");

DEFINE_string(redis_host, "127.0.0.1", "Redis 服务器访问地址");
DEFINE_int32(redis_port, 6379, "Redis 服务器访问端口");
DEFINE_int32(redis_db, 0, "Redis 选择的库");
DEFINE_bool(redis_keep_alive, true, "Redis 长连接");
DEFINE_int32(redis_pool_size, 4, "Redis 连接池大小");

DEFINE_string(mysql_host, "127.0.0.1", "MySQL服务器访问地址");
DEFINE_string(mysql_user, "root", "MySQL访问服务器用户名");
DEFINE_string(mysql_pswd, "", "MySQL服务器访问密码");
DEFINE_string(mysql_db, "chatnow", "MySQL默认库名称");
DEFINE_string(mysql_cset, "utf8mb4", "MySQL客户端字符集");
DEFINE_int32(mysql_port, 0, "MySQL服务器访问端口");
DEFINE_int32(mysql_pool_count, 4, "MySQL连接池最大连接数量");

DEFINE_string(public_url_prefix, "http://127.0.0.1:9000/chatnow-media-public",
              "Media 公开 bucket URL 前缀（avatar_file_id → URL 转换用）");

int main(int argc, char *argv[])
{
    google::ParseCommandLineFlags(&argc, &argv, true);
    chatnow::init_logger(FLAGS_run_mode, FLAGS_log_file, FLAGS_log_level);

    chatnow::ConversationServerBuilder csb;
    csb.make_es_object({FLAGS_es_host});
    csb.make_redis_object(FLAGS_redis_host, FLAGS_redis_port, FLAGS_redis_db,
                          FLAGS_redis_keep_alive, FLAGS_redis_pool_size);
    csb.make_mysql_object(FLAGS_mysql_user, FLAGS_mysql_pswd, FLAGS_mysql_host,
                          FLAGS_mysql_db, FLAGS_mysql_cset,
                          FLAGS_mysql_port, FLAGS_mysql_pool_count);
    csb.make_discovery_object(FLAGS_registry_host, FLAGS_base_service,
                              FLAGS_identity_service, FLAGS_media_service,
                              FLAGS_message_service);
    csb.make_config(FLAGS_public_url_prefix);
    csb.make_rpc_object(FLAGS_listen_port, FLAGS_rpc_timeout, FLAGS_rpc_threads);
    csb.make_registry_object(FLAGS_registry_host,
                             FLAGS_base_service + FLAGS_instance_name,
                             FLAGS_access_host);

    auto server = csb.build();
    server->start();
    return 0;
}
