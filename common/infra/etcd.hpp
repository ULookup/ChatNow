#pragma once

/**
 * ===========================================================================
 * etcd 服务注册 / 发现封装
 * ---------------------------------------------------------------------------
 * 设计要点：
 *   1. Registry: 改用 lease keep-alive；析构走 Cancel（旧版调 Lease() 是错的）
 *   2. Discovery: 监听 basedir 下的 PUT/DELETE 事件，回调送给 ServiceManager
 *   3. 日志统一改为 LOG_xxx 宏（旧版用了 SPDLOG_xxx 不走我们的格式）
 *   4. 新增 Registry::unregister()：服务退出前主动撤销，避免依赖 lease 过期
 *   5. Grant 30-second leases, renew every 9 seconds, and recover failed leases.
 * ===========================================================================
 */

#include <etcd/Client.hpp>
#include <etcd/KeepAlive.hpp>
#include <etcd/Response.hpp>
#include <etcd/Watcher.hpp>
#include <etcd/Value.hpp>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include "infra/logger.hpp"

namespace chatnow
{

inline constexpr int kLeaseSeconds = 30;  // 30s lease，避免 3s 过短的瞬时抖动让节点频繁掉线

/* brief: 服务注册客户端
 *  - 启动时调 registry(key, host) 写入实例信息
 *  - lease 周期内由后台 keep-alive 自动续期
 *  - 进程退出前应主动调 unregister() 撤销，让上游服务发现立即感知下线
 */
class Registry
{
public:
    using ptr = std::shared_ptr<Registry>;

    explicit Registry(const std::string &host)
        : _client(std::make_shared<etcd::Client>(host)) {
        _client->set_grpc_timeout(std::chrono::seconds(2));
        _lease = make_lease_();
        _recovery_thread = std::thread([this] { recover_(); });
    }

    ~Registry() { unregister(); }

    /* brief: 注册服务实例 (key, value=host) */
    bool registry(const std::string &key, const std::string &val) {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_stopping) return false;
        try {
            auto resp = _client->put(key, val, _lease.id).get();
            if(!resp.is_ok()) {
                LOG_ERROR("注册数据失败 key={}: {}", key, resp.error_message());
                return false;
            }
            _registered_key = key;
            _registered_value = val;
            return true;
        } catch(std::exception &e) {
            LOG_ERROR("注册数据异常 key={}: {}", key, e.what());
            return false;
        }
    }

    // Stop recovery before removing the owned registration. Revoke only our
    // lease, so shutdown cannot delete a replacement instance's newer value.
    void unregister() noexcept {
        try {
            std::call_once(_shutdown_once, [this] {
                {
                    std::lock_guard<std::mutex> lock(_mutex);
                    _stopping = true;
                    _registered_key.clear();
                    _registered_value.clear();
                }
                _wake.notify_all();
                if (_recovery_thread.joinable()) _recovery_thread.join();
                discard_lease_(_lease);
            });
        } catch (...) {}
    }

private:
    struct LeaseState {
        int64_t id{0};
        std::shared_ptr<std::atomic<bool>> failed;
        std::shared_ptr<etcd::KeepAlive> keep_alive;
    };

    LeaseState make_lease_() {
        auto granted = _client->leasegrant(kLeaseSeconds).get();
        if (!granted.is_ok() || granted.value().lease() == 0) {
            throw std::runtime_error("registry lease grant failed");
        }
        LeaseState lease;
        lease.id = granted.value().lease();
        lease.failed = std::make_shared<std::atomic<bool>>(false);
        try {
            auto failed = lease.failed;
            // The pinned client waits ttl-1 between renewals. Grant a 30-second
            // lease separately, then renew every 9 seconds to retain headroom.
            // Its failure callback touches only this lease's atomic flag; it
            // never cancels/joins its own thread or publishes a registration.
            lease.keep_alive = std::make_shared<etcd::KeepAlive>(
                *_client, [failed](std::exception_ptr) { failed->store(true); },
                kLeaseSeconds / 3, lease.id);
        } catch (...) {
            discard_lease_(lease);
            throw;
        }
        return lease;
    }

    void discard_lease_(LeaseState &lease) noexcept {
        try { if (lease.keep_alive) lease.keep_alive->Cancel(); } catch (...) {}
        try { if (lease.id != 0) _client->leaserevoke(lease.id).get(); } catch (...) {}
    }

    void recover_() {
        std::unique_lock<std::mutex> lock(_mutex);
        while (!_stopping) {
            _wake.wait_for(lock, std::chrono::seconds(1), [this] { return _stopping; });
            if (_stopping) break;
            if (_registered_key.empty() || !_lease.failed->load()) continue;
            try {
                _lease.keep_alive->Cancel();
                auto replacement = make_lease_();
                try {
                    auto response = _client->put(_registered_key, _registered_value, replacement.id).get();
                    if (!response.is_ok()) throw std::runtime_error("registry recovery put failed");
                } catch (...) {
                    discard_lease_(replacement);
                    throw;
                }
                auto previous = std::move(_lease);
                _lease = std::move(replacement);
                discard_lease_(previous);
                LOG_INFO("registry_recovery outcome=restored");
            } catch (const std::exception &) {
                LOG_WARN("registry_recovery outcome=retry");
            }
        }
    }

    std::shared_ptr<etcd::Client> _client;
    LeaseState _lease;
    std::string _registered_key;
    std::string _registered_value;
    std::mutex _mutex;
    std::condition_variable _wake;
    bool _stopping{false};
    std::once_flag _shutdown_once;
    std::thread _recovery_thread;
};

/* brief: 服务发现客户端
 *  - 启动时拉一次 basedir 下全部 key
 *  - 之后通过 Watcher 监听 PUT/DELETE 事件
 *  - 把事件回调给 ServiceManager 维护 brpc Channel 池
 */
class Discovery
{
public:
    using ptr = std::shared_ptr<Discovery>;
    using NotifyCallback = std::function<void(std::string, std::string)>;

    Discovery(const std::string &host,
              const std::string &basedir,
              const NotifyCallback &put_cb,
              const NotifyCallback &del_cb)
        : _client(std::make_shared<etcd::Client>(host)),
          _basedir(basedir),
          _put_cb(put_cb), _del_cb(del_cb)
    {
        full_sync_();
        // 2) 长轮询监听增量事件
        _watcher = std::make_shared<etcd::Watcher>(
            *_client.get(), basedir,
            std::bind(&Discovery::callback, this, std::placeholders::_1),
            true);
        // 3) 定时全量刷新兜底：Watcher 长连接断开可能丢事件，每隔 kRefreshSec 全量 ls 一次
        _refresh_running = true;
        _refresh_thread = std::thread([this]() {
            while (_refresh_running) {
                std::this_thread::sleep_for(std::chrono::seconds(kRefreshSec));
                if (!_refresh_running) break;
                try {
                    full_sync_();
                } catch (std::exception &e) {
                    LOG_ERROR("Discovery 定时刷新异常: {}", e.what());
                }
            }
        });
    }

    ~Discovery() {
        _refresh_running = false;
        if (_refresh_thread.joinable()) _refresh_thread.join();
        try { if(_watcher) _watcher->Cancel(); } catch(...) {}
    }

private:
    static constexpr int kRefreshSec = 30;  // 每 30s 全量 ls 一次，补齐丢掉的增量事件

    void full_sync_() {
        auto resp = _client->ls(_basedir).get();
        if(!resp.is_ok()) {
            LOG_ERROR("拉取 etcd basedir={} 失败: {}", _basedir, resp.error_message());
            return;
        }
        for(int i = 0; i < static_cast<int>(resp.keys().size()); ++i) {
            if(_put_cb) _put_cb(resp.key(i), resp.value(i).as_string());
        }
    }

    void callback(const etcd::Response &resp) {
        if(!resp.is_ok()) {
            LOG_ERROR("收到错误的事件通知: {}", resp.error_message());
            return;
        }
        for(const auto &ev : resp.events()) {
            if(ev.event_type() == etcd::Event::EventType::PUT) {
                if(_put_cb) _put_cb(ev.kv().key(), ev.kv().as_string());
                LOG_DEBUG("服务上线: {} - {}", ev.kv().key(), ev.kv().as_string());
            } else if(ev.event_type() == etcd::Event::EventType::DELETE_) {
                if(_del_cb) _del_cb(ev.prev_kv().key(), ev.prev_kv().as_string());
                LOG_DEBUG("服务下线: {} - {}", ev.prev_kv().key(), ev.prev_kv().as_string());
            }
        }
    }

    NotifyCallback _put_cb;
    NotifyCallback _del_cb;
    std::string _basedir;
    std::shared_ptr<etcd::Client> _client;
    std::shared_ptr<etcd::Watcher> _watcher;
    std::atomic<bool> _refresh_running{false};
    std::thread _refresh_thread;
};

} // namespace chatnow
