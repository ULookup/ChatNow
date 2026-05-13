#pragma once

#include <websocketpp/config/asio_no_tls.hpp>
#include <websocketpp/server.hpp>
#include "logger.hpp"
#include <mutex>
#include <unordered_map>

namespace chatnow
{

typedef websocketpp::server<websocketpp::config::asio> server_t;

/**
 * Push 服务的连接表（单实例内存，多实例间通过 Redis OnlineRoute 协调路由）。
 * 区别于 Gateway 旧版：
 *   - 多设备：一个 uid 可以挂多个 conn（同一个用户多端登录）
 *   - 增加最后心跳时间戳，便于 reaper 定期清理僵尸连接
 */
class Connection
{
public:
    using ptr = std::shared_ptr<Connection>;

    struct Client {
        std::string uid;
        std::string ssid;
        std::string device_id;
        long last_active_ts {0};   // 心跳更新
        // M2: per-conn 发送串行化锁。websocketpp::connection::send 不是线程安全，
        //     MQ 消费线程 / brpc IO 线程 / WS asio 线程多源并发 send 会撕帧。
        //     用 shared_ptr 让 Connection 拷贝/move 安全，所有持有同一 conn 的拷贝共享同一把锁。
        std::shared_ptr<std::mutex> send_mu {std::make_shared<std::mutex>()};
    };

    Connection() = default;
    ~Connection() = default;

    void insert(const server_t::connection_ptr &conn,
                const std::string &uid,
                const std::string &ssid,
                const std::string &device_id)
    {
        std::unique_lock<std::mutex> lock(_mutex);
        _uid_connections[uid].insert(conn);
        Client c{uid, ssid, device_id, now_sec()};
        _conn_clients[conn] = std::move(c);
        LOG_DEBUG("Connection.insert {} uid={} ssid={} device={}",
                  (size_t)conn.get(), uid, ssid, device_id);
    }

    /* brief: 取该 uid 在本实例上的所有连接 */
    std::vector<server_t::connection_ptr> connections(const std::string &uid) {
        std::unique_lock<std::mutex> lock(_mutex);
        std::vector<server_t::connection_ptr> res;
        auto it = _uid_connections.find(uid);
        if(it == _uid_connections.end()) return res;
        res.reserve(it->second.size());
        for(const auto &c : it->second) res.push_back(c);
        return res;
    }

    bool client(const server_t::connection_ptr &conn,
                std::string &uid, std::string &ssid, std::string &device_id) {
        std::unique_lock<std::mutex> lock(_mutex);
        auto it = _conn_clients.find(conn);
        if(it == _conn_clients.end()) return false;
        uid = it->second.uid;
        ssid = it->second.ssid;
        device_id = it->second.device_id;
        return true;
    }

    /* brief: 取该 conn 的发送串行化锁；连接已不存在则返回 nullptr */
    std::shared_ptr<std::mutex> send_mutex(const server_t::connection_ptr &conn) {
        std::unique_lock<std::mutex> lock(_mutex);
        auto it = _conn_clients.find(conn);
        if(it == _conn_clients.end()) return nullptr;
        return it->second.send_mu;
    }

    void touch(const server_t::connection_ptr &conn) {
        std::unique_lock<std::mutex> lock(_mutex);
        auto it = _conn_clients.find(conn);
        if(it != _conn_clients.end()) it->second.last_active_ts = now_sec();
    }

    void remove(const server_t::connection_ptr &conn) {
        std::unique_lock<std::mutex> lock(_mutex);
        auto it = _conn_clients.find(conn);
        if(it == _conn_clients.end()) return;
        const std::string &uid = it->second.uid;
        auto uc = _uid_connections.find(uid);
        if(uc != _uid_connections.end()) {
            uc->second.erase(conn);
            if(uc->second.empty()) _uid_connections.erase(uc);
        }
        _conn_clients.erase(it);
    }

    /* brief: 收集本实例所有在线 uid（路由表续约用） */
    std::vector<std::string> online_uids() {
        std::unique_lock<std::mutex> lock(_mutex);
        std::vector<std::string> res;
        res.reserve(_uid_connections.size());
        for(const auto &p : _uid_connections) res.push_back(p.first);
        return res;
    }

    /* brief: 清理僵尸连接：last_active_ts 超过 ttl 秒未更新 → 移除
     *  - close handler 已经在大多数场景清理；reaper 是最后的安全网
     *  - 返回被清理的 (uid, conn) 列表，调用方可同步 unbind 路由
     */
    std::vector<std::pair<std::string, server_t::connection_ptr>> reap(long ttl_sec) {
        std::vector<std::pair<std::string, server_t::connection_ptr>> reaped;
        long now = now_sec();
        std::unique_lock<std::mutex> lock(_mutex);
        for(auto it = _conn_clients.begin(); it != _conn_clients.end(); ) {
            if(now - it->second.last_active_ts > ttl_sec) {
                const std::string uid = it->second.uid;
                auto conn = it->first;
                auto uc = _uid_connections.find(uid);
                if(uc != _uid_connections.end()) {
                    uc->second.erase(conn);
                    if(uc->second.empty()) _uid_connections.erase(uc);
                }
                reaped.emplace_back(uid, conn);
                it = _conn_clients.erase(it);
            } else {
                ++it;
            }
        }
        return reaped;
    }
private:
    static long now_sec() {
        using namespace std::chrono;
        return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
    }

    std::mutex _mutex;
    std::unordered_map<std::string,
        std::unordered_set<server_t::connection_ptr>> _uid_connections;
    std::unordered_map<server_t::connection_ptr, Client> _conn_clients;
};

} // namespace chatnow
