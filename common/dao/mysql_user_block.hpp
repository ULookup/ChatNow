#pragma once

#include "infra/logger.hpp"
#include "dao/mysql.hpp"
#include "user_block.hxx"
#include "user_block-odb.hxx"
#include <odb/database.hxx>
#include <odb/mysql/database.hxx>

#include <vector>
#include <string>
#include <unordered_set>
#include <memory>

namespace chatnow
{

/**
 * UserBlockTable
 * ------------------------------------------------------------------
 * user_block 表 DAO 封装。
 * 仅服务于 RelationshipService 的 BlockUser/UnblockUser/ListBlockedUsers
 * 与 SendFriendRequest/SearchFriends 的过滤路径。
 * ------------------------------------------------------------------
 */
class UserBlockTable
{
public:
    using ptr = std::shared_ptr<UserBlockTable>;
    explicit UserBlockTable(const std::shared_ptr<odb::core::database> &db) : _db(db) {}

    /* brief: 写一行；唯一索引冲突视为幂等成功 */
    bool insert(const std::string &blocker, const std::string &blocked) {
        try {
            UserBlock b(blocker, blocked);
            b.create_time(boost::posix_time::microsec_clock::universal_time());
            odb::transaction trans(_db->begin());
            _db->persist(b);
            trans.commit();
            return true;
        } catch (const odb::object_already_persistent &) {
            return true; // 幂等：已经拉黑过
        } catch (const odb::database_exception &e) {
            // MySQL 唯一索引冲突走 database_exception 路径
            std::string what = e.what();
            if (what.find("Duplicate") != std::string::npos) return true;
            LOG_ERROR("插入 user_block 失败 {}-{}: {}", blocker, blocked, what);
            return false;
        } catch (const std::exception &e) {
            LOG_ERROR("插入 user_block 失败 {}-{}: {}", blocker, blocked, e.what());
            return false;
        }
    }

    /* brief: 删除拉黑；不存在返回 true */
    bool remove(const std::string &blocker, const std::string &blocked) {
        try {
            odb::transaction trans(_db->begin());
            using query = odb::query<UserBlock>;
            _db->erase_query<UserBlock>(
                query::blocker_id == blocker && query::blocked_id == blocked);
            trans.commit();
            return true;
        } catch (const std::exception &e) {
            LOG_ERROR("删除 user_block 失败 {}-{}: {}", blocker, blocked, e.what());
            return false;
        }
    }

    /* brief: blocker 是否拉黑了 blocked */
    bool is_blocked(const std::string &blocker, const std::string &blocked) {
        try {
            odb::transaction trans(_db->begin());
            using query = odb::query<UserBlock>;
            std::shared_ptr<UserBlock> r(_db->query_one<UserBlock>(
                query::blocker_id == blocker && query::blocked_id == blocked));
            trans.commit();
            return r != nullptr;
        } catch (const std::exception &e) {
            LOG_ERROR("查询 user_block 失败 {}-{}: {}", blocker, blocked, e.what());
            return false;
        }
    }

    /* brief: blocker 拉黑列表（按 create_time 倒序，limit/offset 分页） */
    std::vector<std::string> list_blocked(const std::string &blocker, int offset, int limit) {
        std::vector<std::string> res;
        if (limit <= 0) return res;
        try {
            odb::transaction trans(_db->begin());
            using query  = odb::query<UserBlock>;
            using result = odb::result<UserBlock>;
            std::string tail = " ORDER BY create_time DESC LIMIT " +
                               std::to_string(limit) +
                               " OFFSET " + std::to_string(offset < 0 ? 0 : offset);
            result r(_db->query<UserBlock>(
                (query::blocker_id == blocker) + tail));
            for (auto &row : r) res.push_back(row.blocked_id());
            trans.commit();
        } catch (const std::exception &e) {
            LOG_ERROR("列举 user_block 失败 {}: {}", blocker, e.what());
        }
        return res;
    }

    /* brief: blocker 拉黑总数 */
    int64_t count_blocked(const std::string &blocker) {
        try {
            odb::transaction trans(_db->begin());
            using query = odb::query<UserBlock>;
            using result = odb::result<UserBlock>;
            int64_t cnt = 0;
            result r(_db->query<UserBlock>(query::blocker_id == blocker));
            for (auto it = r.begin(); it != r.end(); ++it) ++cnt;
            trans.commit();
            return cnt;
        } catch (const std::exception &e) {
            LOG_ERROR("统计 user_block 失败 {}: {}", blocker, e.what());
            return 0;
        }
    }

    /* brief: "我拉黑的 ∪ 拉黑我的" 全集；用于 SearchFriends 过滤 */
    std::unordered_set<std::string> blocked_or_blocking(const std::string &uid) {
        std::unordered_set<std::string> res;
        try {
            odb::transaction trans(_db->begin());
            using query  = odb::query<UserBlock>;
            using result = odb::result<UserBlock>;
            // 我拉黑的人
            result r1(_db->query<UserBlock>(query::blocker_id == uid));
            for (auto &row : r1) res.insert(row.blocked_id());
            // 拉黑我的人
            result r2(_db->query<UserBlock>(query::blocked_id == uid));
            for (auto &row : r2) res.insert(row.blocker_id());
            trans.commit();
        } catch (const std::exception &e) {
            LOG_ERROR("blocked_or_blocking 查询失败 {}: {}", uid, e.what());
        }
        return res;
    }

private:
    std::shared_ptr<odb::core::database> _db;
};

} // namespace chatnow
