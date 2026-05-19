#pragma once

#include <etcd/Client.hpp>
#include <etcd/KeepAlive.hpp>
#include <etcd/Transaction.hpp>
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
        if (_keep_alive) {
            try { _keep_alive->Cancel(); } catch (...) {}
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

                etcd::Transaction txn;
                txn.setup_compare_version(_key, etcd::CompareResult::EQUAL, 0);
                txn.setup_put_success(_key, _id, lease_id);
                txn.setup_get_failure(_key);
                auto txn_resp = _etcd->txn(txn).get();

                if (txn_resp.is_ok() && txn_resp.value().succeeded()) {
                    _keep_alive = _etcd->keepalive(lease_id).get();
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

    void _hold_leadership_(int64_t lease_id) {
        while (_running && _is_leader) {
            if (!_sleep_interruptible_(std::chrono::seconds(1))) return;
            auto ttl_resp = _etcd->timetolive(lease_id).get();
            if (!ttl_resp.is_ok() || ttl_resp.value().ttl() <= 0) {
                LOG_WARN("LeaderElection lease {} 过期，失去 leader", lease_id);
                break;
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
