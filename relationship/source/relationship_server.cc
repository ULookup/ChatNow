#include "relationship_server.h"

DEFINE_bool(run_mode, false, "程序的运行模式 false-调试 ; true-发布");
DEFINE_string(log_file, "", "发布模式下，用于指定日志的输出文件");
DEFINE_int32(log_level, 0, "发布模式下，用于指定日志的输出等级");

DEFINE_string(registry_host, "http://127.0.0.1:2379", "服务注册中心地址");
DEFINE_string(base_service, "/service", "服务监控根目录");
DEFINE_string(instance_name, "/relationship_service/instance", "服务实例 etcd 路径");
DEFINE_string(access_host, "127.0.0.1:10006", "当前实例的外部访问地址");

DEFINE_int32(listen_port, 10006, "RPC服务器监听端口");
DEFINE_int32(rpc_timeout, -1, "RPC调用超时时间");
DEFINE_int32(rpc_threads, 1, "RPC的IO线程数量");

DEFINE_string(identity_service, "/service/identity_service", "Identity 子服务名称（GetMultiUserInfo）");
DEFINE_string(conversation_service, "/service/conversation_service", "Conversation 子服务名称（CreateConversation）");

DEFINE_string(es_host, "http://127.0.0.1:9200/", "ES搜索引擎服务器URL");

DEFINE_string(mysql_host, "127.0.0.1", "MySQL服务器访问地址");
DEFINE_string(mysql_user, "root", "MySQL访问服务器用户名");
DEFINE_string(mysql_pswd, "", "MySQL服务器访问密码");
DEFINE_string(mysql_db, "chatnow", "MySQL默认库名称");
DEFINE_string(mysql_cset, "utf8mb4", "MySQL客户端字符集");
DEFINE_int32(mysql_port, 0, "MySQL服务器访问端口");
DEFINE_int32(mysql_pool_count, 4, "MySQL连接池最大连接数量");

int main(int argc, char *argv[])
{
    google::ParseCommandLineFlags(&argc, &argv, true);
    chatnow::init_logger(FLAGS_run_mode, FLAGS_log_file, FLAGS_log_level);

    chatnow::RelationshipServerBuilder rsb;
    rsb.make_es_object({FLAGS_es_host});
    rsb.make_mysql_object(FLAGS_mysql_user, FLAGS_mysql_pswd, FLAGS_mysql_host,
                          FLAGS_mysql_db, FLAGS_mysql_cset,
                          FLAGS_mysql_port, FLAGS_mysql_pool_count);
    rsb.make_discovery_object(FLAGS_registry_host, FLAGS_base_service,
                              FLAGS_identity_service, FLAGS_conversation_service);
    rsb.make_rpc_object(FLAGS_listen_port, FLAGS_rpc_timeout, FLAGS_rpc_threads);
    rsb.make_registry_object(FLAGS_registry_host,
                             FLAGS_base_service + FLAGS_instance_name,
                             FLAGS_access_host);

    auto server = rsb.build();
    server->start();
    return 0;
}
