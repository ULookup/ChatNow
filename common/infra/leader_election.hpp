#pragma once

#include <etcd/Client.hpp>
#include <etcd/KeepAlive.hpp>
#include <etcd/v3/Transaction.hpp>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include "infra/logger.hpp"

namespace chatnow {

class LeaderElection {
public:
    using ptr = std::shared_ptr<LeaderElection>;

    LeaderElection(std::shared_ptr<etcd::Client> etcd,
                   const std::string &election_key,
                   const std::string &instance_id,
                   int lease_ttl_sec,
                   std::function<void()> on_acquired,
                   std::function<void()> on_lost)
        : _etcd(std::move(etcd)), _key(election_key), _id(instance_id),
          _ttl(lease_ttl_sec), _on_acquired(std::move(on_acquired)),
          _on_lost(std::move(on_lost)) {}

    ~LeaderElection() { stop(); }

    void start() {
        _running = true;
        _thread = std::thread([this]() { campaign_loop_(); });
    }

    void stop() {
        _running = false;
        _cv.notify_all();
        {
            std::lock_guard<std::mutex> lk(_cv_mu);
            if (_keep_alive) {
                try { _keep_alive->Cancel(); } catch (...) {}
            }
        }
        if (_thread.joinable()) _thread.join();
    }

    bool is_leader() const { return _is_leader.load(); }

private:
    void campaign_loop_() {
        while (_running) {
            try {
                auto lease_resp = _etcd->leasegrant(_ttl).get();
                if (!lease_resp.is_ok()) {
                    LOG_WARN("LeaderElection leasegrant 失败: {}", lease_resp.error_message());
                    if (!_sleep_interruptible_(std::chrono::seconds(_ttl / 2))) return;
                    continue;
                }
                int64_t lease_id = lease_resp.value().lease();

                etcdv3::Transaction txn;
                txn.add_compare_version(_key, etcdv3::CompareResult::EQUAL, 0);
                txn.add_success_put(_key, _id, lease_id);
                txn.add_failure_range(_key);
                auto txn_resp = _etcd->txn(txn).get();

                if (txn_resp.is_ok() && txn_resp.values().empty()) {
                    {
                        std::lock_guard<std::mutex> lk(_cv_mu);
                        _keep_alive = std::make_shared<etcd::KeepAlive>(*_etcd, _ttl, lease_id);
                    }
                    _is_leader = true;
                    if (_on_acquired) _on_acquired();

                    _hold_leadership_(lease_id);

                    if (_is_leader.exchange(false)) {
                        try { _keep_alive->Cancel(); } catch (...) {}
                        if (_on_lost) _on_lost();
                    }
                } else {
                    LOG_DEBUG("LeaderElection: {} 已被占用，等待重试", _key);
                    try { _etcd->leaserevoke(lease_id).wait(); } catch (...) {}
                }
            } catch (std::exception &e) {
                LOG_ERROR("LeaderElection campaign 异常: {}", e.what());
            }

            if (!_sleep_interruptible_(std::chrono::seconds(_ttl / 3))) return;
        }
    }

    static constexpr int kMaxTtlFailures = 5;  // 连续 5 次 TTL 检查失败才放弃

    void _hold_leadership_(int64_t lease_id) {
        int consecutive_failures = 0;
        while (_running && _is_leader) {
            if (!_sleep_interruptible_(std::chrono::seconds(1))) return;
            try {
                auto ttl_resp = _etcd->leasetimetolive(lease_id).get();
                if (!ttl_resp.is_ok() || ttl_resp.value().ttl() <= 0) {
                    consecutive_failures++;
                    LOG_WARN("LeaderElection lease {} TTL 检查失败 ({}/{})",
                             lease_id, consecutive_failures, kMaxTtlFailures);
                    if (consecutive_failures >= kMaxTtlFailures) {
                        LOG_ERROR("LeaderElection lease {} 连续 {} 次检查失败，放弃 leader",
                                  lease_id, kMaxTtlFailures);
                        break;
                    }
                } else {
                    consecutive_failures = 0;  // 成功则重置计数器
                }
            } catch (std::exception &e) {
                consecutive_failures++;
                LOG_WARN("LeaderElection lease {} TTL 查询异常 ({}/{}): {}",
                         lease_id, consecutive_failures, kMaxTtlFailures, e.what());
                if (consecutive_failures >= kMaxTtlFailures) break;
            }
        }
    }

    bool _sleep_interruptible_(std::chrono::seconds duration) {
        std::unique_lock<std::mutex> lk(_cv_mu);
        return !_cv.wait_for(lk, duration, [this] { return !_running; });
    }

    std::shared_ptr<etcd::Client> _etcd;
    std::string _key;
    std::string _id;
    int _ttl;
    std::function<void()> _on_acquired;
    std::function<void()> _on_lost;

    std::thread _thread;
    std::atomic<bool> _running{false};
    std::atomic<bool> _is_leader{false};
    std::shared_ptr<etcd::KeepAlive> _keep_alive;

    std::mutex _cv_mu;
    std::condition_variable _cv;
};

} // namespace chatnow
