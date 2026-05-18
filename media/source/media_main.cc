// MediaServer 启动入口
//   1. 解析 gflags
//   2. 初始化 logger
//   3. 加载 conf/media.json 拿 s3 / media 段
//   4. Aws::InitAPI
//   5. 用 Builder 组装 MediaServer 并 start

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

#include <aws/core/Aws.h>
#include <gflags/gflags.h>
#include <json/json.h>

#include "media_server.h"
#include "infra/logger.hpp"
#include "utils/mime_whitelist.hpp"

DEFINE_bool(run_mode, false, "程序的运行模式 false-调试 ; true-发布");
DEFINE_string(log_file, "", "发布模式下，用于指定日志的输出文件");
DEFINE_int32(log_level, 0, "发布模式下，用于指定日志的输出等级");

DEFINE_string(registry_host, "http://127.0.0.1:2379", "服务注册中心地址");
DEFINE_string(base_service, "/service", "服务监控根目录");
DEFINE_string(instance_name, "/media_service/instance", "服务实例名");
DEFINE_string(access_host, "127.0.0.1:10002", "当前实例对外访问地址");

DEFINE_int32(listen_port, 10002, "RPC 服务器监听端口");
DEFINE_int32(rpc_timeout, -1, "RPC 调用超时");
DEFINE_int32(rpc_threads, 4, "RPC 的 IO 线程数");

DEFINE_string(mysql_host, "127.0.0.1", "MySQL 地址");
DEFINE_string(mysql_user, "root", "MySQL 用户名");
DEFINE_string(mysql_pswd, "", "MySQL 密码 (通过 --mysql_pswd 或 MYSQL_PSWD 环境变量设置)");
DEFINE_string(mysql_db,   "chatnow", "MySQL 库");
DEFINE_string(mysql_cset, "utf8mb4", "MySQL 字符集");
DEFINE_int32 (mysql_port, 0, "MySQL 端口");
DEFINE_int32 (mysql_pool_count, 4, "MySQL 连接池");

DEFINE_string(redis_host, "127.0.0.1", "Redis 地址");
DEFINE_int32 (redis_port, 6379, "Redis 端口");
DEFINE_int32 (redis_db,   0, "Redis 默认库号");
DEFINE_bool  (redis_keep_alive, true, "Redis 长连接");

DEFINE_string(media_conf, "/im/conf/media.json", "媒体配置文件路径(JSON：s3 + media 段)");

namespace {

struct LoadedMediaConf {
    chatnow::MediaServiceConfig                  cfg;
    std::shared_ptr<chatnow::MimeWhitelist>      mime;
    std::string s3_endpoint;
    std::string s3_region;
    std::string s3_access_key;
    std::string s3_secret_key;
};

LoadedMediaConf load_media_conf(const std::string& path) {
    std::ifstream ifs(path);
    if (!ifs) throw std::runtime_error("media_conf: cannot open " + path);

    Json::Value root;
    Json::CharReaderBuilder b;
    std::string err;
    if (!Json::parseFromStream(b, ifs, &root, &err)) {
        throw std::runtime_error("media_conf: parse failed: " + err);
    }
    if (!root.isMember("s3") || !root.isMember("media")) {
        throw std::runtime_error("media_conf: missing s3 or media section in " + path);
    }
    const auto& s3 = root["s3"];
    const auto& md = root["media"];

    LoadedMediaConf out;
    out.s3_endpoint   = s3.get("endpoint",   "").asString();
    out.s3_region     = s3.get("region",     "us-east-1").asString();
    out.s3_access_key = s3.get("access_key", "").asString();
    out.s3_secret_key = s3.get("secret_key", "").asString();

    out.cfg.public_bucket     = md.get("public_bucket",     "").asString();
    out.cfg.private_bucket    = md.get("private_bucket",    "").asString();
    out.cfg.public_url_prefix = md.get("public_url_prefix", "").asString();
    out.cfg.presign_seconds   = md.get("presign_seconds",   900).asInt();
    out.cfg.asr_endpoint      = md.get("asr_endpoint",      "").asString();

    if (!md.isMember("mime_whitelist") || !md["mime_whitelist"].isArray()) {
        throw std::runtime_error("media_conf: missing media.mime_whitelist array");
    }
    out.mime = std::make_shared<chatnow::MimeWhitelist>();
    if (!out.mime->load_value(md["mime_whitelist"])) {
        throw std::runtime_error("media_conf: media.mime_whitelist load failed");
    }
    return out;
}

}  // namespace

int main(int argc, char* argv[]) {
    google::ParseCommandLineFlags(&argc, &argv, true);
    chatnow::init_logger(FLAGS_run_mode, FLAGS_log_file, FLAGS_log_level);

    // 密码优先从环境变量读取，命令行参数次之
    if (FLAGS_mysql_pswd.empty()) {
        const char* env = std::getenv("MYSQL_PSWD");
        if (env && *env) FLAGS_mysql_pswd = env;
    }
    if (FLAGS_mysql_pswd.empty()) {
        std::cerr << "mysql_pswd 必须通过 --mysql_pswd 或环境变量 MYSQL_PSWD 设置" << std::endl;
        return 1;
    }

    LoadedMediaConf conf;
    try {
        conf = load_media_conf(FLAGS_media_conf);
    } catch (const std::exception& e) {
        std::cerr << "media_conf 加载失败: " << e.what() << std::endl;
        return 1;
    }

    Aws::SDKOptions sdk;
    Aws::InitAPI(sdk);

    {
        chatnow::MediaServerBuilder b;
        b.make_mysql_object(FLAGS_mysql_user, FLAGS_mysql_pswd, FLAGS_mysql_host,
                            FLAGS_mysql_db, FLAGS_mysql_cset, FLAGS_mysql_port,
                            FLAGS_mysql_pool_count);
        b.make_redis_object(FLAGS_redis_host, FLAGS_redis_port, FLAGS_redis_db,
                            FLAGS_redis_keep_alive);
        b.make_s3_object(conf.s3_endpoint, conf.s3_region,
                         conf.s3_access_key, conf.s3_secret_key);
        b.set_media_config(conf.cfg, conf.mime);
        b.make_registry_object(FLAGS_registry_host,
                               FLAGS_base_service + FLAGS_instance_name,
                               FLAGS_access_host);
        b.make_rpc_object(FLAGS_listen_port, FLAGS_rpc_timeout, FLAGS_rpc_threads);

        auto server = b.build();
        server->start();
    }

    Aws::ShutdownAPI(sdk);
    return 0;
}
