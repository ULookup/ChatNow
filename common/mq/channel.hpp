#pragma once

/**
 * ===========================================================================
 * brpc 服务信道管理（配合 etcd 服务发现）
 * ---------------------------------------------------------------------------
 * 设计要点：
 *   1. _index 改为 std::atomic 保证 RR 计数无竞争
 *   2. ChannelOptions 增加合理超时（旧版 -1 一直等待，会拖垮调用线程）：
 *      - connect_timeout: 1s
 *      - timeout:         3s
 *   3. ServiceManager.choose() 在没有可用节点时返回 nullptr 并打 warn，
 *      调用方需显式判空
 *   4. 容器使用 unordered_map / unordered_set，保留就近的"添加→使用"读
 * ===========================================================================
 */

#include <atomic>
#include <brpc/channel.h>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "infra/logger.hpp"

namespace chatnow
{

inline constexpr int32_t kConnectTimeoutMs = 1000;   // 连接超时 1s
inline constexpr int32_t kRpcTimeoutMs     = 3000;   // 单次 RPC 超时 3s
inline constexpr int32_t kRpcMaxRetry      = 3;

/* brief: 单服务多实例信道集合（同一服务名下的多个节点） */
class ServiceChannel
{
public:
    using ptr = std::shared_ptr<ServiceChannel>;
    using ChannelPtr = std::shared_ptr<brpc::Channel>;

    explicit ServiceChannel(const std::string &name) : _service_name(name), _index(0) {}

    /* brief: 节点上线，新增 brpc::Channel */
    void append(const std::string &host) {
        auto channel = std::make_shared<brpc::Channel>();
        brpc::ChannelOptions options;
        options.connect_timeout_ms = kConnectTimeoutMs;
        options.timeout_ms         = kRpcTimeoutMs;
        options.max_retry          = kRpcMaxRetry;
        options.protocol           = "baidu_std";
        if(channel->Init(host.c_str(), &options) != 0) {
            LOG_ERROR("初始化 {}-{} 信道失败", _service_name, host);
            return;
        }
        std::unique_lock<std::mutex> lock(_mutex);
        _hosts[host] = channel;
        _channels.push_back(channel);
    }

    /* brief: 节点下线，释放 Channel */
    void remove(const std::string &host) {
        std::unique_lock<std::mutex> lock(_mutex);
        auto it = _hosts.find(host);
        if(it == _hosts.end()) {
            LOG_WARN("{}-{} 节点删除信道时未找到信道信息", _service_name, host);
            return;
        }
        for(auto vit = _channels.begin(); vit != _channels.end(); ++vit) {
            if(*vit == it->second) {
                _channels.erase(vit);
                break;
            }
        }
        _hosts.erase(it);
    }

    /* brief: RR 选择一个 channel；空集合返回 nullptr */
    ChannelPtr choose() {
        std::unique_lock<std::mutex> lock(_mutex);
        if(_channels.empty()) {
            LOG_ERROR("当前没有能提供 {} 服务的节点", _service_name);
            return ChannelPtr();
        }
        auto idx = _index.fetch_add(1, std::memory_order_relaxed) % _channels.size();
        return _channels[idx];
    }

    /* brief: 当前节点数 — 健康检查/监控用 */
    size_t size() const {
        std::unique_lock<std::mutex> lock(_mutex);
        return _channels.size();
    }

private:
    mutable std::mutex _mutex;
    std::atomic<uint32_t> _index;
    std::string _service_name;
    std::vector<ChannelPtr> _channels;
    std::unordered_map<std::string, ChannelPtr> _hosts;
};

/* brief: 多服务的 Channel 总管 — 配合 etcd Discovery 维护 */
class ServiceManager
{
public:
    using ptr = std::shared_ptr<ServiceManager>;
    ServiceManager() = default;

    /* brief: 选中目标服务的某个节点 channel */
    ServiceChannel::ChannelPtr choose(const std::string &service_name) {
        std::unique_lock<std::mutex> lock(_mutex);
        auto it = _services.find(service_name);
        if(it == _services.end()) {
            LOG_ERROR("当前没有能提供 {} 服务的节点", service_name);
            return ServiceChannel::ChannelPtr();
        }
        return it->second->choose();
    }

    /* brief: 声明关注哪些服务（不关注的服务上下线事件会被忽略，节省内存） */
    void declared(const std::string &service_name) {
        std::unique_lock<std::mutex> lock(_mutex);
        _follow_services.insert(service_name);
    }

    /* brief: etcd PUT 事件回调 */
    void onServiceOnline(const std::string &service_instance, const std::string &host) {
        std::string service_name = getServiceName(service_instance);
        ServiceChannel::ptr service;
        {
            std::unique_lock<std::mutex> lock(_mutex);
            if(_follow_services.find(service_name) == _follow_services.end()) {
                LOG_DEBUG("{}-{} 服务上线，但当前不关心", service_name, host);
                return;
            }
            auto it = _services.find(service_name);
            if(it == _services.end()) {
                service = std::make_shared<ServiceChannel>(service_name);
                _services[service_name] = service;
            } else {
                service = it->second;
            }
        }
        LOG_DEBUG("{}-{} 服务上线新节点", service_name, host);
        service->append(host);
    }

    /* brief: etcd DELETE 事件回调 */
    void onServiceOffline(const std::string &service_instance, const std::string &host) {
        std::string service_name = getServiceName(service_instance);
        ServiceChannel::ptr service;
        {
            std::unique_lock<std::mutex> lock(_mutex);
            auto it = _services.find(service_name);
            if(it == _services.end()) {
                LOG_WARN("删除 {} 服务节点时未找到管理对象", service_name);
                return;
            }
            service = it->second;
        }
        LOG_DEBUG("{}-{} 服务下线节点", service_name, host);
        service->remove(host);
    }

private:
    /* brief: 从注册路径 /service/xxx/instance/<id> 中抽出服务名 /service/xxx/instance */
    std::string getServiceName(const std::string &service_instance) {
        auto pos = service_instance.find_last_of('/');
        if(pos == std::string::npos) return service_instance;
        return service_instance.substr(0, pos);
    }

    std::mutex _mutex;
    std::unordered_set<std::string> _follow_services;
    std::unordered_map<std::string, ServiceChannel::ptr> _services;
};

} // namespace chatnow
