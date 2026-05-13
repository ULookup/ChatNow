#pragma once

#include "logger.hpp"
#include "mysql.hpp"
#include "message_read.hxx"
#include "message_read-odb.hxx"
#include <odb/database.hxx>
#include <odb/mysql/database.hxx>

#include <memory>
#include <string>
#include <vector>

namespace chatnow
{

/**
 * MessageReadTable
 * ------------------------------------------------------------------
 * message_read 表的 DAO 封装（群消息已读回执）。
 *
 * 注意写入路径：
 *   - 高 QPS 写入应走 Redis 暂存（key: read:{message_id}）+ 后台批量异步刷库
 *   - 本 DAO 是落库接口；ack() 单条写仅用于低频/调试场景
 *   - 大群消息发出 24h 后停止收集 ack（产品策略）
 *
 * 索引使用：
 *   - uk_msg_user (message_id, user_id) — 唯一约束兜底防重复
 *   - idx_msg_time (message_id, read_time) — 已读列表分页（按时间倒序）
 * ------------------------------------------------------------------
 */
class MessageReadTable
{
public:
    using ptr = std::shared_ptr<MessageReadTable>;
    MessageReadTable(const std::shared_ptr<odb::core::database> &db) : _db(db) {}

    /* brief: 单条 ack（重复写依赖 uk_msg_user 唯一约束） */
    bool ack(unsigned long message_id, const std::string &user_id) {
        try {
            MessageRead mr(message_id, user_id, boost::posix_time::microsec_clock::universal_time());
            odb::transaction trans(_db->begin());
            _db->persist(mr);
            trans.commit();
        } catch(std::exception &e) {
            // 唯一冲突视为幂等成功，不上报错误
            LOG_DEBUG("已读 ack 写入 {}/{}: {}", message_id, user_id, e.what());
            return false;
        }
        return true;
    }

    /* brief: 批量 ack — 后台批处理刷库主路径 */
    bool ack_batch(std::vector<MessageRead> &acks) {
        if(acks.empty()) return true;
        try {
            odb::transaction trans(_db->begin());
            for(auto &mr : acks) {
                try {
                    _db->persist(mr);
                } catch(odb::exception &) {
                    // 单行重复忽略，不让整批回滚
                }
            }
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("批量已读 ack 失败 count={}: {}", acks.size(), e.what());
            return false;
        }
        return true;
    }

    /* brief: 取一条消息的"已读名单"（按 read_time 倒序分页） */
    std::vector<MessageRead> readers_of(unsigned long message_id, size_t limit = 200) {
        std::vector<MessageRead> res;
        try {
            odb::transaction trans(_db->begin());
            using query  = odb::query<MessageRead>;
            using result = odb::result<MessageRead>;
            result r(_db->query<MessageRead>(
                (query::message_id == message_id) +
                " ORDER BY read_time DESC LIMIT " + std::to_string(limit)));
            for(auto &row : r) res.push_back(row);
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("查询已读列表失败 mid={}: {}", message_id, e.what());
        }
        return res;
    }

    /* brief: 已读人数（角标"已读 X 人"显示用） */
    int reader_count(unsigned long message_id) {
        int count = 0;
        try {
            odb::transaction trans(_db->begin());
            using query  = odb::query<MessageRead>;
            using result = odb::result<MessageRead>;
            result r(_db->query<MessageRead>(query::message_id == message_id));
            for(auto it = r.begin(); it != r.end(); ++it) ++count;
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("查询已读数失败 mid={}: {}", message_id, e.what());
        }
        return count;
    }

    /* brief: 我是否已读某条消息 */
    bool has_read(unsigned long message_id, const std::string &user_id) {
        try {
            odb::transaction trans(_db->begin());
            using query = odb::query<MessageRead>;
            std::shared_ptr<MessageRead> r(_db->query_one<MessageRead>(
                query::message_id == message_id && query::user_id == user_id));
            trans.commit();
            return r != nullptr;
        } catch(std::exception &e) {
            LOG_ERROR("查询已读状态失败 mid={} uid={}: {}", message_id, user_id, e.what());
            return false;
        }
    }

private:
    std::shared_ptr<odb::core::database> _db;
};

} // namespace chatnow
