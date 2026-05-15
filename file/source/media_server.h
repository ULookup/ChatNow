#pragma once

/**
 * MediaServer + MediaServerBuilder + MediaServiceImpl
 * ---
 * 仿 user_server.h 的 Builder 模式。组件构造顺序：
 *   make_mysql_object → make_redis_object → make_s3_object →
 *   make_media_config（加载 conf/media.json） →
 *   make_registry_object → make_rpc_object → build → start
 *
 * MediaServiceImpl 在本头文件内 inline 实现 RPC override（仅作转发，
 * 业务逻辑落在 *_handler.hpp 中）。
 *
 * cleanup_worker 由 MediaServer 持有，start() 时启动后台线程，析构时停止。
 */

#include <brpc/server.h>
#include <butil/logging.h>
#include <memory>
#include <string>

#include <odb/database.hxx>
#include <odb/mysql/database.hxx>
#include <sw/redis++/redis++.h>

#include "auth/auth_context.hpp"
#include "dao/data_redis.hpp"
#include "dao/mysql.hpp"
#include "error/error_codes.hpp"
#include "error/handle_rpc.hpp"
#include "infra/etcd.hpp"
#include "infra/logger.hpp"
#include "infra/s3_client.hpp"
#include "media/media_service.pb.h"
#include "utils/mime_whitelist.hpp"

// 各 handler 在后续 task 中创建（Task 14–22）；本文件依赖如下头部
#include "cleanup_worker.hpp"     // Task 22
#include "download_handler.hpp"   // Task 20
#include "multipart_handler.hpp"  // Task 17–19
#include "speech_handler.hpp"     // Task 20
#include "upload_handler.hpp"     // Task 14–15
#include "dao/mysql_media_blob_ref.hpp"
#include "dao/mysql_media_file.hpp"
#include "dao/mysql_media_user_quota.hpp"

namespace chatnow {

struct MediaServiceConfig {
    std::string public_bucket;
    std::string private_bucket;
    std::string public_url_prefix;
    int         presign_seconds {900};
    std::string asr_endpoint;
};

class MediaServiceImpl : public ::chatnow::media::MediaService {
public:
    MediaServiceImpl(std::shared_ptr<S3Client> s3,
                     std::shared_ptr<MimeWhitelist> mime,
                     std::shared_ptr<odb::core::database> mysql,
                     std::shared_ptr<sw::redis::Redis> /*redis*/,
                     const MediaServiceConfig& cfg)
    {
        auto files  = std::make_shared<MediaFileTable>(mysql);
        auto blobs  = std::make_shared<MediaBlobRefTable>(mysql);
        auto quota  = std::make_shared<MediaUserQuotaTable>(mysql);

        _upload   = std::make_unique<UploadHandler>(
                       s3, mime, files, blobs, quota,
                       cfg.public_bucket, cfg.private_bucket, cfg.presign_seconds);
        _multi    = std::make_unique<MultipartHandler>(
                       s3, mime, files, blobs, quota,
                       cfg.public_bucket, cfg.private_bucket, cfg.presign_seconds);
        _download = std::make_unique<DownloadHandler>(s3, files, cfg.presign_seconds);
        _speech   = std::make_unique<SpeechHandler>(cfg.asr_endpoint);
    }

    // RPC override 在 Task 16 / 21 接入；此处先 forward decl 以便编译。
    void ApplyUpload            (::google::protobuf::RpcController*,
                                 const ::chatnow::media::ApplyUploadReq*,
                                 ::chatnow::media::ApplyUploadRsp*,
                                 ::google::protobuf::Closure*) override;
    void CompleteUpload         (::google::protobuf::RpcController*,
                                 const ::chatnow::media::CompleteUploadReq*,
                                 ::chatnow::media::CompleteUploadRsp*,
                                 ::google::protobuf::Closure*) override;
    void InitMultipartUpload    (::google::protobuf::RpcController*,
                                 const ::chatnow::media::InitMultipartReq*,
                                 ::chatnow::media::InitMultipartRsp*,
                                 ::google::protobuf::Closure*) override;
    void ApplyPartUpload        (::google::protobuf::RpcController*,
                                 const ::chatnow::media::ApplyPartReq*,
                                 ::chatnow::media::ApplyPartRsp*,
                                 ::google::protobuf::Closure*) override;
    void CompleteMultipartUpload(::google::protobuf::RpcController*,
                                 const ::chatnow::media::CompleteMultipartReq*,
                                 ::chatnow::media::CompleteMultipartRsp*,
                                 ::google::protobuf::Closure*) override;
    void AbortMultipartUpload   (::google::protobuf::RpcController*,
                                 const ::chatnow::media::AbortMultipartReq*,
                                 ::chatnow::media::AbortMultipartRsp*,
                                 ::google::protobuf::Closure*) override;
    void ApplyDownload          (::google::protobuf::RpcController*,
                                 const ::chatnow::media::ApplyDownloadReq*,
                                 ::chatnow::media::ApplyDownloadRsp*,
                                 ::google::protobuf::Closure*) override;
    void GetFileInfo            (::google::protobuf::RpcController*,
                                 const ::chatnow::media::GetFileInfoReq*,
                                 ::chatnow::media::GetFileInfoRsp*,
                                 ::google::protobuf::Closure*) override;
    void SpeechRecognition      (::google::protobuf::RpcController*,
                                 const ::chatnow::media::SpeechRecognitionReq*,
                                 ::chatnow::media::SpeechRecognitionRsp*,
                                 ::google::protobuf::Closure*) override;

private:
    std::unique_ptr<UploadHandler>    _upload;
    std::unique_ptr<MultipartHandler> _multi;
    std::unique_ptr<DownloadHandler>  _download;
    std::unique_ptr<SpeechHandler>    _speech;
};

class MediaServer {
public:
    using ptr = std::shared_ptr<MediaServer>;

    MediaServer(Registry::ptr reg,
                std::shared_ptr<odb::core::database> mysql,
                std::shared_ptr<sw::redis::Redis> redis,
                std::shared_ptr<S3Client> s3,
                std::shared_ptr<brpc::Server> server,
                std::unique_ptr<CleanupWorker> worker)
        : _reg(std::move(reg)),
          _mysql(std::move(mysql)),
          _redis(std::move(redis)),
          _s3(std::move(s3)),
          _rpc_server(std::move(server)),
          _worker(std::move(worker)) {}

    ~MediaServer() { if (_worker) _worker->stop(); }

    void start() {
        if (_worker) _worker->start();
        _rpc_server->RunUntilAskedToQuit();
    }

private:
    Registry::ptr                          _reg;
    std::shared_ptr<odb::core::database>   _mysql;
    std::shared_ptr<sw::redis::Redis>      _redis;
    std::shared_ptr<S3Client>              _s3;
    std::shared_ptr<brpc::Server>          _rpc_server;
    std::unique_ptr<CleanupWorker>         _worker;
};

class MediaServerBuilder {
public:
    void make_mysql_object(const std::string& user, const std::string& password,
                           const std::string& host, const std::string& db,
                           const std::string& cset, uint16_t port, int pool_count) {
        _mysql = ODBFactory::create(user, password, host, db, cset, port, pool_count);
    }

    void make_redis_object(const std::string& host, uint16_t port, int db, bool keep_alive) {
        _redis = RedisClientFactory::create(host, port, db, keep_alive);
    }

    void make_s3_object(const std::string& endpoint, const std::string& region,
                        const std::string& access_key, const std::string& secret_key) {
        S3Options o{endpoint, region, access_key, secret_key, /*path_style*/true};
        _s3 = std::make_shared<S3Client>(o);
    }

    void make_media_config(const std::string& media_conf_path) {
        // 在 media_main.cc 中调用：解析 conf/media.json，填 _cfg + 加载 mime 白名单
        _media_conf_path = media_conf_path;
    }

    void set_media_config(const MediaServiceConfig& cfg, std::shared_ptr<MimeWhitelist> mime) {
        _cfg = cfg;
        _mime = std::move(mime);
    }

    void make_registry_object(const std::string& reg_host,
                              const std::string& service_name,
                              const std::string& access_host) {
        _reg = std::make_shared<Registry>(reg_host);
        _reg->registry(service_name, access_host);
    }

    void make_rpc_object(uint16_t port, uint32_t timeout, uint8_t num_threads) {
        _rpc_server = std::make_shared<brpc::Server>();
        if (!_mysql || !_redis || !_s3 || !_mime) {
            LOG_ERROR("MediaServer: 必要组件未初始化（mysql/redis/s3/mime）");
            abort();
        }

        auto* media_service = new MediaServiceImpl(_s3, _mime, _mysql, _redis, _cfg);
        if (_rpc_server->AddService(media_service, brpc::ServiceOwnership::SERVER_OWNS_SERVICE) == -1) {
            LOG_ERROR("AddService MediaService 失败");
            abort();
        }

        brpc::ServerOptions opt;
        opt.idle_timeout_sec = timeout;
        opt.num_threads      = num_threads;
        if (_rpc_server->Start(port, &opt) == -1) {
            LOG_ERROR("RPC server 启动失败");
            abort();
        }
    }

    MediaServer::ptr build() {
        if (!_reg || !_rpc_server) {
            LOG_ERROR("MediaServer: registry/rpc 未初始化");
            abort();
        }
        auto worker = std::make_unique<CleanupWorker>(
            _s3, _mysql, _redis, _cfg.public_bucket, _cfg.private_bucket);
        return std::make_shared<MediaServer>(
            _reg, _mysql, _redis, _s3, _rpc_server, std::move(worker));
    }

private:
    std::shared_ptr<odb::core::database> _mysql;
    std::shared_ptr<sw::redis::Redis>    _redis;
    std::shared_ptr<S3Client>            _s3;
    std::shared_ptr<MimeWhitelist>       _mime;
    Registry::ptr                        _reg;
    std::shared_ptr<brpc::Server>        _rpc_server;
    MediaServiceConfig                   _cfg;
    std::string                          _media_conf_path;
};

}  // namespace chatnow
