#pragma once

/**
 * CleanupWorker —— 媒体子系统后台清理（单线程）
 * ---
 * 4 类任务：
 *   - task_pending_timeout    ：1h 未 CompleteUpload 的 pending 行 → deleted
 *   - task_multipart_orphan   ：bucket 内 24h 无活动的 multipart upload → abort
 *   - task_unref_blob_gc      ：ref_count==0 + 7 天 GC 缓冲后 → 真正删 MinIO 对象 + 退 quota
 *   - task_quarantine_gc      ：quarantined 7 天后 → 真删
 *   - task_magic_sniff        ：committed 行 Range GET 前 512B → 与 mime 不匹配置 quarantined
 *
 * 单实例租约：im:media:gc:lease（SET NX EX 600，TTL 内续期）
 *
 * 任务实现见 Task 23 / 24 / 25。
 */

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <odb/database.hxx>
#include <sw/redis++/redis++.h>

#include "dao/mysql_media_blob_ref.hpp"
#include "dao/mysql_media_file.hpp"
#include "dao/mysql_media_user_quota.hpp"
#include "infra/logger.hpp"
#include "infra/s3_client.hpp"
#include "infra/snowflake.hpp"
#include "upload_handler.hpp"   // next_file_id_hex 用作 instance_id
#include "utils/magic_sniff.hpp"

namespace chatnow {

inline constexpr const char* kMediaGcLeaseKey = "im:media:gc:lease";
inline constexpr int          kMediaGcLeaseTtlSec = 600;
inline constexpr int          kMediaGcLeaseRenewIntervalMs = 200'000;

class CleanupWorker {
public:
    CleanupWorker(CleanupWorker&&) = delete;
    CleanupWorker& operator=(CleanupWorker&&) = delete;

    CleanupWorker(std::shared_ptr<S3Client>            s3,
                  std::shared_ptr<odb::core::database> mysql,
                  std::shared_ptr<sw::redis::Redis>    redis,
                  std::string                          public_bucket,
                  std::string                          private_bucket)
        : _s3(std::move(s3)),
          _redis(std::move(redis)),
          _files(std::make_shared<MediaFileTable>(mysql)),
          _blobs(std::make_shared<MediaBlobRefTable>(mysql)),
          _quota(std::make_shared<MediaUserQuotaTable>(mysql)),
          _pub_b(std::move(public_bucket)),
          _pri_b(std::move(private_bucket)),
          _instance_id(next_file_id_hex()) {}

    ~CleanupWorker() { stop(); }

    void start() {
        bool expected = false;
        if (!_running.compare_exchange_strong(expected, true)) return;
        _stop = false;
        _t = std::thread([this] { run(); });
    }

    void stop() {
        _stop = true;
        bool expected = true;
        if (!_running.compare_exchange_strong(expected, false)) return;
        if (_t.joinable()) _t.join();
        // 释放租约（best effort）
        try_release_lease();
    }

private:
    void run() {
        int64_t last_pending     = 0;
        int64_t last_orphan      = 0;
        int64_t last_blob_gc     = 0;
        int64_t last_quarantine  = 0;
        int64_t last_sniff       = 0;
        int64_t last_renew       = 0;

        while (!_stop) {
            int64_t now = now_ms();
            if (!try_acquire_lease()) { sleep_ms(5000); continue; }

            if (now - last_renew    > kMediaGcLeaseRenewIntervalMs)         { renew_lease();          last_renew      = now; }
            if (now - last_pending  > 5LL  * 60'000)                        { task_pending_timeout(); last_pending    = now; }
            if (now - last_orphan   > 60LL * 60'000)                        { task_multipart_orphan();last_orphan     = now; }
            if (now - last_blob_gc  > 24LL * 60 * 60'000)                   { task_unref_blob_gc();   last_blob_gc    = now; }
            if (now - last_quarantine > 24LL * 60 * 60'000)                 { task_quarantine_gc();  last_quarantine = now; }
            if (now - last_sniff    > 60'000)                               { task_magic_sniff();    last_sniff      = now; }

            sleep_ms(10'000);
        }
    }

    // ---- lease ----
    bool try_acquire_lease() {
        static const char* kAcquireLua =
            "if redis.call('SET', KEYS[1], ARGV[1], 'NX', 'EX', ARGV[2]) then return 1 end "
            "if redis.call('GET', KEYS[1]) == ARGV[1] then "
            "    redis.call('EXPIRE', KEYS[1], ARGV[2]); return 1 "
            "end "
            "return 0";
        try {
            std::vector<std::string> keys = {kMediaGcLeaseKey};
            std::vector<std::string> args = {_instance_id, std::to_string(kMediaGcLeaseTtlSec)};
            auto ret = _redis->eval<long long>(kAcquireLua, keys.begin(), keys.end(),
                                               args.begin(), args.end());
            return ret == 1;
        } catch (std::exception& e) {
            LOG_ERROR("media gc lease acquire 失败: {}", e.what());
            return false;
        }
    }

    void renew_lease() {
        // try_acquire_lease 自带续期路径；额外续期靠下一轮调用
        try_acquire_lease();
    }

    void try_release_lease() {
        static const char* kReleaseLua =
            "if redis.call('GET', KEYS[1]) == ARGV[1] then "
            "    return redis.call('DEL', KEYS[1]) "
            "end "
            "return 0";
        try {
            std::vector<std::string> keys = {kMediaGcLeaseKey};
            std::vector<std::string> args = {_instance_id};
            _redis->eval<long long>(kReleaseLua, keys.begin(), keys.end(),
                                    args.begin(), args.end());
        } catch (...) {}
    }

    // ---- 各任务实现 ---- 见下方 inline definitions（Task 23 / 24 / 25 覆盖）
    void task_pending_timeout();
    void task_multipart_orphan();
    void task_unref_blob_gc();
    void task_quarantine_gc();
    void task_magic_sniff();

    static int64_t now_ms() {
        using namespace std::chrono;
        return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
    }
    static void sleep_ms(int ms) {
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    }

    std::shared_ptr<S3Client>             _s3;
    std::shared_ptr<sw::redis::Redis>     _redis;
    std::shared_ptr<MediaFileTable>       _files;
    std::shared_ptr<MediaBlobRefTable>    _blobs;
    std::shared_ptr<MediaUserQuotaTable>  _quota;
    std::string                           _pub_b, _pri_b;
    std::string                           _instance_id;

    std::atomic<bool>                     _running{false};
    std::atomic<bool>                     _stop{true};
    std::thread                           _t;
};

// ===== 任务实现 =====

/* brief: 1h 未 CompleteUpload 的 pending 行 → deleted；若 blob ref_count==0 顺手删 S3 对象 */
inline void CleanupWorker::task_pending_timeout() {
    auto cutoff = boost::posix_time::microsec_clock::universal_time()
                - boost::posix_time::hours(1);
    auto rows = _files->list_pending_older_than(cutoff, /*limit*/200);
    int cleaned = 0;
    for (auto& r : rows) {
        // 若 blob 没有人引用（即从未 Complete 过），删 S3 对象
        auto blob = _blobs->select_by_hash(r.content_hash());
        if (!blob || blob->ref_count() == 0) {
            try { _s3->delete_object(r.bucket(), r.object_key()); } catch (...) {}
        }
        // multipart 孤儿同时 abort
        if (!r.upload_id().empty()) {
            try { _s3->abort_multipart(r.bucket(), r.object_key(), r.upload_id()); } catch (...) {}
        }
        _files->update_status(r.file_id(), MediaFileStatus::DELETED);
        ++cleaned;
    }
    if (cleaned > 0) LOG_INFO("gc_pending_timeout cleaned={}", cleaned);
}

/* brief: bucket 内 24h 无活动的 multipart upload → abort + 行 deleted */
inline void CleanupWorker::task_multipart_orphan() {
    auto cutoff_ms = now_ms() - 24LL * 60 * 60'000;
    int aborted = 0;
    for (const auto& bucket : { _pub_b, _pri_b }) {
        std::vector<S3Client::OrphanUpload> ups;
        try { ups = _s3->list_multipart_uploads(bucket); } catch (...) { continue; }
        for (auto& u : ups) {
            if (u.initiated_ms > cutoff_ms) continue;
            try { _s3->abort_multipart(bucket, u.key, u.upload_id); ++aborted; }
            catch (...) {}
            // 关联 file 行（可能已经 deleted，select 不到也无所谓）
            auto file = _files->select_by_upload_id(u.upload_id);
            if (file && file->status() == MediaFileStatus::PENDING) {
                _files->update_status(file->file_id(), MediaFileStatus::DELETED);
            }
        }
    }
    if (aborted > 0) LOG_INFO("gc_multipart_orphan aborted={}", aborted);
}

/* brief: ref_count==0 + 7 天 GC 缓冲 → 真删 MinIO 对象 + 退 quota */
inline void CleanupWorker::task_unref_blob_gc() {
    auto cutoff = boost::posix_time::microsec_clock::universal_time()
                - boost::posix_time::hours(24 * 7);
    auto rows = _blobs->list_zero_ref_older_than(cutoff, /*limit*/200);
    int    cleaned = 0;
    int64_t total_size = 0;
    auto now = boost::posix_time::microsec_clock::universal_time();
    for (auto& b : rows) {
        try { _s3->delete_object(b.bucket(), b.object_key()); }
        catch (...) {
            LOG_WARN("gc_unref_blob: delete s3 object 失败 hash={}", b.content_hash());
            continue;
        }
        // 退还相关 owner 的 quota（按 status in {DELETED, QUARANTINED} 的关联 file 行）
        auto orphans = _files->list_files_by_hash_dead(b.content_hash());
        for (auto& f : orphans) {
            _quota->dec_used(f.owner_id(), b.total_size(), now);
        }
        _blobs->erase(b.content_hash());
        total_size += b.total_size();
        ++cleaned;
    }
    if (cleaned > 0) LOG_INFO("gc_unref_blob cleaned={} total_size={}", cleaned, total_size);
}

/* brief: status=QUARANTINED + 7 天 → 真删 + 行 deleted */
inline void CleanupWorker::task_quarantine_gc() {
    auto cutoff = boost::posix_time::microsec_clock::universal_time()
                - boost::posix_time::hours(24 * 7);
    auto rows = _files->list_quarantined_older_than(cutoff, /*limit*/200);
    int cleaned = 0;
    for (auto& r : rows) {
        try { _s3->delete_object(r.bucket(), r.object_key()); } catch (...) {}
        _files->update_status(r.file_id(), MediaFileStatus::DELETED);
        ++cleaned;
    }
    if (cleaned > 0) LOG_INFO("gc_quarantine cleaned={}", cleaned);
}

/* brief: committed 行扫游标后续：Range GET 前 512B → mime 不匹配则置 quarantined
 *
 * 为避免改 schema，cursor 仅作进程内变量；重启后从 now-10min 开始回扫，
 * 重复 sniff 同一 committed 行无副作用（幂等）。
 */
inline void CleanupWorker::task_magic_sniff() {
    static thread_local boost::posix_time::ptime cursor =
        boost::posix_time::microsec_clock::universal_time()
        - boost::posix_time::minutes(10);

    auto rows = _files->list_committed_after(cursor, /*limit*/100);
    int sniffed = 0;
    int quarantined = 0;
    for (auto& r : rows) {
        try {
            auto buf = _s3->get_range(r.bucket(), r.object_key(), 512);
            if (!magic_sniff::matches_claimed(buf, r.mime_type())) {
                _files->update_status(r.file_id(), MediaFileStatus::QUARANTINED);
                ++quarantined;
                LOG_WARN("quarantined file={} mime={} reason=magic_mismatch",
                         r.file_id(), r.mime_type());
            }
            ++sniffed;
        } catch (...) {
            LOG_WARN("sniff failed file={} key={}", r.file_id(), r.object_key());
        }
        if (r.uploaded_at() > cursor) cursor = r.uploaded_at();
    }
    if (sniffed > 0 || quarantined > 0) {
        LOG_INFO("gc_magic_sniff sniffed={} quarantined={}", sniffed, quarantined);
    }
}

}  // namespace chatnow
