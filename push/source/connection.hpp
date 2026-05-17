#pragma once

#include <websocketpp/config/asio_no_tls.hpp>
#include <websocketpp/server.hpp>
#include "infra/logger.hpp"
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace chatnow
{

typedef websocketpp::server<websocketpp::config::asio> server_t;

/**
 * Push 服务连接表 — 设备级路由。
 *   - _uid_device_connections: uid → device_id → set<conn_ptr>
 *   - _conn_clients:           conn → Client{uid, device_id, jwt_jti, last_active_ts, send_mu}
 *   - 同一 (uid, device_id) 有新连接时关闭旧连接
 */
class Connection
{
public:
    using ptr = std::shared_ptr<Connection>;

    struct Client {
        std::string uid;
        std::string device_id;
        std::string jwt_jti;
        long last_active_ts {0};
        std::shared_ptr<std::mutex> send_mu {std::make_shared<std::mutex>()};
    };

    Connection() = default;
    ~Connection() = default;

    /* brief: 插入连接。同 (uid, device_id) 已有则关闭旧连接后替换 */
    void insert(const server_t::connection_ptr &conn,
                const std::string &uid,
                const std::string &device_id,
                const std::string &jwt_jti)
    {
        std::unique_lock<std::mutex> lock(_mutex);
        // 关闭同一设备的旧连接
        auto dit = _uid_device_connections.find(uid);
        if (dit != _uid_device_connections.end()) {
            auto vdit = dit->second.find(device_id);
            if (vdit != dit->second.end()) {
                for (const auto &old_conn : vdit->second) {
                    try { old_conn->close(websocketpp::close::status::normal, "new device login"); }
                    catch(...) {}
                    _conn_clients.erase(old_conn);
                }
                vdit->second.clear();
            }
        }
        _uid_device_connections[uid][device_id].insert(conn);
        Client c{uid, device_id, jwt_jti, now_sec()};
        _conn_clients[conn] = std::move(c);
        LOG_DEBUG("Connection.insert {} uid={} device={}",
                  (size_t)conn.get(), uid, device_id);
    }

    /* brief: 取指定设备的连接 */
    std::vector<server_t::connection_ptr> connections(const std::string &uid,
                                                      const std::string &device_id) {
        std::unique_lock<std::mutex> lock(_mutex);
        std::vector<server_t::connection_ptr> res;
        auto dit = _uid_device_connections.find(uid);
        if (dit == _uid_device_connections.end()) return res;
        auto vdit = dit->second.find(device_id);
        if (vdit == dit->second.end()) return res;
        res.reserve(vdit->second.size());
        for (const auto &c : vdit->second) res.push_back(c);
        return res;
    }

    bool client(const server_t::connection_ptr &conn,
                std::string &uid, std::string &device_id, std::string &jti) {
        std::unique_lock<std::mutex> lock(_mutex);
        auto it = _conn_clients.find(conn);
        if (it == _conn_clients.end()) return false;
        uid = it->second.uid;
        device_id = it->second.device_id;
        jti = it->second.jwt_jti;
        return true;
    }

    std::shared_ptr<std::mutex> send_mutex(const server_t::connection_ptr &conn) {
        std::unique_lock<std::mutex> lock(_mutex);
        auto it = _conn_clients.find(conn);
        if (it == _conn_clients.end()) return nullptr;
        return it->second.send_mu;
    }

    void touch(const server_t::connection_ptr &conn) {
        std::unique_lock<std::mutex> lock(_mutex);
        auto it = _conn_clients.find(conn);
        if (it != _conn_clients.end()) it->second.last_active_ts = now_sec();
    }

    void remove(const server_t::connection_ptr &conn) {
        std::unique_lock<std::mutex> lock(_mutex);
        auto it = _conn_clients.find(conn);
        if (it == _conn_clients.end()) return;
        const std::string &uid = it->second.uid;
        const std::string &did = it->second.device_id;
        auto dit = _uid_device_connections.find(uid);
        if (dit != _uid_device_connections.end()) {
            auto vdit = dit->second.find(did);
            if (vdit != dit->second.end()) {
                vdit->second.erase(conn);
                if (vdit->second.empty()) dit->second.erase(vdit);
            }
            if (dit->second.empty()) _uid_device_connections.erase(dit);
        }
        _conn_clients.erase(it);
    }

    std::vector<std::string> online_uids() {
        std::unique_lock<std::mutex> lock(_mutex);
        std::vector<std::string> res;
        res.reserve(_uid_device_connections.size());
        for (const auto &p : _uid_device_connections) res.push_back(p.first);
        return res;
    }

    std::vector<std::pair<std::string, server_t::connection_ptr>> reap(long ttl_sec) {
        std::vector<std::pair<std::string, server_t::connection_ptr>> reaped;
        long now = now_sec();
        std::unique_lock<std::mutex> lock(_mutex);
        for (auto cit = _conn_clients.begin(); cit != _conn_clients.end(); ) {
            if (now - cit->second.last_active_ts > ttl_sec) {
                const std::string uid = cit->second.uid;
                auto conn = cit->first;
                auto dit = _uid_device_connections.find(uid);
                if (dit != _uid_device_connections.end()) {
                    auto vdit = dit->second.find(cit->second.device_id);
                    if (vdit != dit->second.end()) {
                        vdit->second.erase(conn);
                        if (vdit->second.empty()) dit->second.erase(vdit);
                    }
                    if (dit->second.empty()) _uid_device_connections.erase(dit);
                }
                reaped.emplace_back(uid, conn);
                cit = _conn_clients.erase(cit);
            } else {
                ++cit;
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
        std::unordered_map<std::string,
            std::unordered_set<server_t::connection_ptr>>> _uid_device_connections;
    std::unordered_map<server_t::connection_ptr, Client> _conn_clients;
};

} // namespace chatnow
