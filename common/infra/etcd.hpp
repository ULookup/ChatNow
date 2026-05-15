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
 *   5. KeepAlive 间隔 3s 写死过短；改为 30s lease + 默认 keepalive 内置心跳
 * ===========================================================================
 */

#include <etcd/Client.hpp>
#include <etcd/KeepAlive.hpp>
#include <etcd/Response.hpp>
#include <etcd/Watcher.hpp>
#include <etcd/Value.hpp>
#include <functional>
#include <memory>
#include <string>
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
        : _client(std::make_shared<etcd::Client>(host)),
          _keep_alive(_client->leasekeepalive(kLeaseSeconds).get()),
          _lease_id(_keep_alive->Lease()) {}

    ~Registry() {
        try {
            // 撤销 lease，关联的 key 立刻失效；旧实现错调 Lease() 实际是 getter
            _keep_alive->Cancel();
            _client->leaserevoke(_lease_id).wait();
        } catch(...) { /* 析构吞异常 */ }
    }

    /* brief: 注册服务实例 (key, value=host) */
    bool registry(const std::string &key, const std::string &val) {
        try {
            auto resp = _client->put(key, val, _lease_id).get();
            if(!resp.is_ok()) {
                LOG_ERROR("注册数据失败 key={}: {}", key, resp.error_message());
                return false;
            }
            _registered_key = key;
            return true;
        } catch(std::exception &e) {
            LOG_ERROR("注册数据异常 key={}: {}", key, e.what());
            return false;
        }
    }

    /* brief: 主动注销 — 进程优雅退出时应在主循环停止后调用 */
    void unregister() {
        if(_registered_key.empty()) return;
        try {
            _client->rm(_registered_key).wait();
            _registered_key.clear();
        } catch(std::exception &e) {
            LOG_WARN("主动注销失败 key={}: {}", _registered_key, e.what());
        }
    }

private:
    std::shared_ptr<etcd::Client> _client;
    std::shared_ptr<etcd::KeepAlive> _keep_alive;
    uint64_t _lease_id;
    std::string _registered_key;  // 注册的 key，析构 / 主动注销时使用
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
          _put_cb(put_cb), _del_cb(del_cb)
    {
        // 1) 全量拉取 basedir 下当前 key，触发 PUT 回调
        auto resp = _client->ls(basedir).get();
        if(!resp.is_ok()) {
            LOG_ERROR("拉取 etcd basedir={} 失败: {}", basedir, resp.error_message());
        } else {
            for(int i = 0; i < static_cast<int>(resp.keys().size()); ++i) {
                if(_put_cb) _put_cb(resp.key(i), resp.value(i).as_string());
            }
        }
        // 2) 长轮询监听增量事件
        _watcher = std::make_shared<etcd::Watcher>(
            *_client.get(), basedir,
            std::bind(&Discovery::callback, this, std::placeholders::_1),
            true);
    }

    ~Discovery() {
        try { if(_watcher) _watcher->Cancel(); } catch(...) {}
    }

private:
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
    std::shared_ptr<etcd::Client> _client;
    std::shared_ptr<etcd::Watcher> _watcher;
};

} // namespace chatnow
