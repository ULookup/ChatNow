#pragma once

/**
 * ===========================================================================
 * Worker ID 自动分配（基于 Redis 租约）
 * ---------------------------------------------------------------------------
 * 用于雪花 ID 生成器：每个 transmite / 其它需要唯一 worker_id 的实例启动时
 * 通过 Redis 申请一个 [0, 1023] 范围内的唯一编号，避免多实例配置撞 ID。
 *
 * 协议：
 *   1. INCR im:worker:{service_name}:seq        得到候选编号 c = (c-1) % 1024
 *   2. SET  im:worker:{service_name}:slot:c {host}:{pid} NX EX 300
 *      - 成功 → 当前实例占用该编号，定时刷新（每 60s）
 *      - 失败 → c++ 重试；最多扫一圈（1024 次）
 *   3. 实例下线时 DEL slot key 释放编号
 *
 * 续期失败处理（防租约误覆盖）：
 *   - EXPIRE 仅在 key 仍归本实例所有时延期；若 key 已被别人占用（本实例租约已失效），
 *     不能用裸 SET 覆盖，只能放弃本编号 → 触发上层报警/退出，避免雪花重号。
 *
 * 兜底：申请失败（Redis 不通）回退到配置传入的 fallback_worker_id。
 * ===========================================================================
 */

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <sw/redis++/redis++.h>
#include "infra/logger.hpp"

namespace chatnow
{

class WorkerIdAllocator
{
public:
    using ptr = std::shared_ptr<WorkerIdAllocator>;

    static constexpr int kMaxWorkerId = 1024;
    static constexpr int kLeaseSec = 300;
    static constexpr int kRenewSec = 60;

    WorkerIdAllocator(const std::shared_ptr<sw::redis::Redis> &c,
                      const std::string &service_name,
                      const std::string &owner)
        : _c(c), _service(service_name), _owner(owner),
          _running(false), _allocated(-1), _lease_lost(false) {}

    ~WorkerIdAllocator() { stop(); }

    /**
     * brief: 申请一个 worker_id；成功返回 [0,1023]，失败返回 fallback_worker_id
     */
    int acquire(int fallback_worker_id) {
        try {
            long base = _c->incr(seq_key());
            int start = static_cast<int>(((base - 1) % kMaxWorkerId + kMaxWorkerId) % kMaxWorkerId);

            for(int i = 0; i < kMaxWorkerId; ++i) {
                int candidate = (start + i) % kMaxWorkerId;
                std::string k = slot_key(candidate);
                using namespace std::chrono;
                auto ok = _c->set(k, _owner, seconds(kLeaseSec),
                                  sw::redis::UpdateType::NOT_EXIST);
                if(ok) {
                    _allocated.store(candidate, std::memory_order_release);
                    LOG_INFO("WorkerIdAllocator: 申请到 worker_id={} for {}", candidate, _service);
                    start_renew_thread();
                    return candidate;
                }
            }
            LOG_ERROR("WorkerIdAllocator: 1024 个 slot 全部被占用，回退到 fallback={}", fallback_worker_id);
        } catch(std::exception &e) {
            LOG_ERROR("WorkerIdAllocator: Redis 异常: {}，回退到 fallback={}", e.what(), fallback_worker_id);
        }
        _allocated.store(fallback_worker_id, std::memory_order_release);
        return fallback_worker_id;
    }

    /* brief: 租约是否已经失效（业务层据此决定是否优雅退出避免雪花重号） */
    bool lease_lost() const { return _lease_lost.load(std::memory_order_acquire); }

    void stop() {
        {
            std::lock_guard<std::mutex> lk(_cv_mutex);
            _running.store(false, std::memory_order_release);
        }
        _cv.notify_all();
        if(_renew_thread.joinable()) _renew_thread.join();
        // 释放 slot（仅当还属于本实例时）
        int allocated = _allocated.load(std::memory_order_acquire);
        if(allocated >= 0 && allocated < kMaxWorkerId && !_lease_lost.load()) {
            try {
                auto cur = _c->get(slot_key(allocated));
                if(cur && *cur == _owner) {
                    _c->del(slot_key(allocated));
                    LOG_INFO("WorkerIdAllocator: 释放 worker_id={} for {}", allocated, _service);
                }
            } catch(std::exception &e) {
                LOG_WARN("WorkerIdAllocator: 释放 slot 失败 {}", e.what());
            }
        }
    }
private:
    std::string seq_key() const { return "im:worker:" + _service + ":seq"; }
    std::string slot_key(int id) const {
        return "im:worker:" + _service + ":slot:" + std::to_string(id);
    }

    void start_renew_thread() {
        _running.store(true, std::memory_order_release);
        _renew_thread = std::thread([this]() {
            while(true) {
                {
                    std::unique_lock<std::mutex> lk(_cv_mutex);
                    if(_cv.wait_for(lk, std::chrono::seconds(kRenewSec),
                                    [this]() { return !_running.load(); })) {
                        break;  // stop 被调用
                    }
                }
                int id = _allocated.load(std::memory_order_acquire);
                if(id < 0 || id >= kMaxWorkerId) continue;
                try {
                    auto cur = _c->get(slot_key(id));
                    if(cur && *cur == _owner) {
                        // 仍然归本实例 → 延期
                        _c->expire(slot_key(id), std::chrono::seconds(kLeaseSec));
                    } else {
                        // 租约已失效 → 标记并退出续期循环；
                        // 不能用裸 SET 覆盖（可能撞别人的租约 → 雪花重号）
                        LOG_ERROR("WorkerIdAllocator: 租约 {} 已失效，停止续期；调用方应停服报警",
                                  id);
                        _lease_lost.store(true, std::memory_order_release);
                        return;
                    }
                } catch(std::exception &e) {
                    LOG_WARN("WorkerIdAllocator: 续期失败（保留租约状态）: {}", e.what());
                }
            }
        });
    }

    std::shared_ptr<sw::redis::Redis> _c;
    std::string _service;
    std::string _owner;
    std::atomic<bool> _running;
    std::atomic<int> _allocated;
    std::atomic<bool> _lease_lost;
    std::mutex _cv_mutex;
    std::condition_variable _cv;
    std::thread _renew_thread;
};

} // namespace chatnow
