#include "gateway_server.h"

DEFINE_bool(run_mode, false, "程序的运行模式 false-调试 ; true-发布");
DEFINE_string(log_file, "", "发布模式下，用于指定日志的输出文件");
DEFINE_int32(log_level, 0, "发布模式下，用于指定日志的输出等级");

DEFINE_int32(http_listen_port, 9000, "HTTP服务器监听端口");

DEFINE_string(registry_host, "http://127.0.0.1:2379", "服务注册中心地址");
DEFINE_string(base_service, "/service", "服务监控根目录");
DEFINE_string(identity_service, "/service/identity_service", "Identity 子服务名称");
DEFINE_string(relationship_service, "/service/relationship_service", "Relationship 子服务名称");
DEFINE_string(conversation_service, "/service/conversation_service", "Conversation 子服务名称");
DEFINE_string(message_service, "/service/message_service", "Message 子服务名称");
DEFINE_string(transmite_service, "/service/transmite_service", "Transmite 子服务名称");
DEFINE_string(media_service, "/service/media_service", "Media 子服务名称");
DEFINE_string(presence_service, "/service/presence_service", "Presence 子服务名称");
DEFINE_string(push_service, "/service/push_service", "Push 子服务名称");

DEFINE_string(redis_host, "127.0.0.1", "Redis服务器访问地址");
DEFINE_int32(redis_port, 6379, "Redis服务器访问端口");
DEFINE_int32(redis_db, 0, "Redis默认库号");
DEFINE_bool(redis_keep_alive, true, "Redis长连接保活");

DEFINE_string(auth_config, "/im/conf/auth.json", "JWT 鉴权配置文件路径(JSON)");

int main(int argc, char *argv[])
{
    google::ParseCommandLineFlags(&argc, &argv, true);
    chatnow::init_logger(FLAGS_run_mode, FLAGS_log_file, FLAGS_log_level);

    chatnow::GatewayServerBuilder gsb;
    gsb.make_redis_object(FLAGS_redis_host, FLAGS_redis_port, FLAGS_redis_db, FLAGS_redis_keep_alive);
    gsb.make_jwt_object(FLAGS_auth_config);
    gsb.make_discovery_object(FLAGS_registry_host, FLAGS_base_service,
                              FLAGS_identity_service, FLAGS_relationship_service,
                              FLAGS_conversation_service, FLAGS_message_service,
                              FLAGS_transmite_service, FLAGS_media_service,
                              FLAGS_presence_service, FLAGS_push_service);
    gsb.make_server_object(FLAGS_http_listen_port);

    auto server = gsb.build();
    server->start();
    return 0;
}
