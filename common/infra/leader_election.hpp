#pragma once

#include <etcd/Client.hpp>
#include <etcd/KeepAlive.hpp>
#include <etcd/Transaction.hpp>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include "infra/logger.hpp"

namespace chatnow {

// etcd Lease + Transaction CAS 选举锁。
// 使用 etcdv3 Transaction 保证"仅当 key 不存在时才写入"，消除双主窗口。
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
                    std::this_thread::sleep_for(std::chrono::seconds(_ttl / 2));
                    continue;
                }
                int64_t lease_id = lease_resp.value().lease();

                // Transaction CAS: compare version(key) == 0 → key 不存在则写入
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

            if (_running) {
                std::this_thread::sleep_for(std::chrono::seconds(_ttl / 3));
            }
        }
    }

    void _hold_leadership_(int64_t lease_id) {
        while (_running && _is_leader) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            auto ttl_resp = _etcd->timetolive(lease_id).get();
            if (!ttl_resp.is_ok() || ttl_resp.value().ttl() <= 0) {
                LOG_WARN("LeaderElection lease {} 过期，失去 leader", lease_id);
                break;
            }
        }
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
};

} // namespace chatnow
