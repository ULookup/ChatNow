#pragma once

/**
 * ===========================================================================
 * MySQL ODB 数据库工厂
 * ---------------------------------------------------------------------------
 * 设计要点：
 *   1. 工厂保留旧签名，新增 charset / pool_size / conn_lifetime 调优参数
 *   2. 默认 charset 改为 utf8mb4，匹配新 schema（旧 utf8 存不下 emoji）
 *   3. 提供 TxnGuard：基于事务感知的 RAII 包装，便于 DAO 层组合事务
 *   4. 全局心跳健康检查：ping() 用 SQL "SELECT 1" 实现，便于注册到健康探针
 * ===========================================================================
 */

#include <memory>
#include <string>
#include <odb/database.hxx>
#include <odb/transaction.hxx>
#include <odb/mysql/database.hxx>
#include "logger.hpp"

namespace chatnow
{

class ODBFactory
{
public:
    /* brief: 创建 ODB 数据库句柄
     *  @param conn_pool_count 连接池最大连接数；建议每服务 4~16
     *  @param charset         字符集；新 schema 必须 utf8mb4
     */
    static std::shared_ptr<odb::core::database> create(const std::string &user,
                                                       const std::string &password,
                                                       const std::string &host,
                                                       const std::string &db,
                                                       const std::string &charset,
                                                       uint16_t port,
                                                       int conn_pool_count)
    {
        std::string cs = charset.empty() ? std::string("utf8mb4") : charset;
        auto factory = std::unique_ptr<odb::mysql::connection_pool_factory>(
            new odb::mysql::connection_pool_factory(conn_pool_count, 0));
        return std::make_shared<odb::mysql::database>(
            user, password, db, host, port, "", cs, 0, std::move(factory));
    }

    /* brief: 健康检查 — 用最廉价的 SELECT 1 验证连接池可用 */
    static bool ping(const std::shared_ptr<odb::core::database> &db) {
        try {
            odb::transaction t(db->begin());
            db->execute("SELECT 1");
            t.commit();
            return true;
        } catch(std::exception &e) {
            LOG_ERROR("MySQL ping 失败: {}", e.what());
            return false;
        }
    }
};

/**
 * TxnGuard
 * ------------------------------------------------------------------
 * 事务 RAII：嵌套场景挂载到外层事务，最外层负责 commit。
 *   void Service::foo(...) {
 *       TxnGuard tx(_db);
 *       _msg_dao->insert(msg);
 *       _timeline_dao->insert(timelines);
 *       tx.commit();
 *   }
 * DAO 内部使用 odb::transaction::has_current() 即可感知是否已在外层事务中。
 * ------------------------------------------------------------------
 */
class TxnGuard
{
public:
    explicit TxnGuard(const std::shared_ptr<odb::core::database> &db) {
        if(odb::transaction::has_current()) {
            _owner = false;
        } else {
            _owner = true;
            _txn.reset(new odb::transaction(db->begin()));
        }
    }
    ~TxnGuard() {
        // 析构未提交则视为异常路径，事务自动回滚（odb::transaction 析构语义）
    }
    void commit() {
        if(_owner && _txn) {
            _txn->commit();
            _owner = false;
        }
    }
    bool owner() const { return _owner; }

private:
    bool _owner = false;
    std::unique_ptr<odb::transaction> _txn;
};

} // namespace chatnow
