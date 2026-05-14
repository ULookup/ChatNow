#pragma once

#include "infra/logger.hpp"
#include "dao/mysql.hpp"
#include "relation.hxx"
#include "relation-odb.hxx"
#include <odb/database.hxx>
#include <odb/mysql/database.hxx>

#include <vector>
#include <string>

namespace chatnow
{

/**
 * RelationTable
 * ------------------------------------------------------------------
 * relation 表（双向冗余）的 DAO 封装。
 *
 * 关键变化：
 *   - 不再物理删除好友：remove() 改为软删除，置 status=DELETED 保留行，
 *     便于风控审计与"加回好友"识别历史关系
 *   - 新增字段相关写入接口：备注 / 分组 / 星标 / 拉黑
 *   - friends() 仅返回 status=NORMAL 的好友，过滤掉拉黑/已删除的对端
 *   - 任何状态变更都自动维护 update_time，客户端按 update_time 增量同步
 * ------------------------------------------------------------------
 */
class RelationTable
{
public:
    using ptr = std::shared_ptr<RelationTable>;
    RelationTable(const std::shared_ptr<odb::core::database> &db) : _db(db) {}

    /* brief: 双向新增好友关系（同意申请时调用） */
    bool insert(const std::string &uid, const std::string &pid) {
        try {
            auto now = boost::posix_time::microsec_clock::universal_time();
            Relation r1(uid, pid);
            Relation r2(pid, uid);
            r1.create_time(now); r1.update_time(now);
            r2.create_time(now); r2.update_time(now);

            odb::transaction trans(_db->begin());
            _db->persist(r1);
            _db->persist(r2);
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("新增用户好友关系失败 {}-{}: {}", uid, pid, e.what());
            return false;
        }
        return true;
    }

    /* brief: 软删除好友关系（双向置 DELETED，保留历史行）
     *  - 不物理 erase，避免破坏申请审计与"加回好友"识别旧关系
     */
    bool remove(const std::string &uid, const std::string &pid) {
        try {
            auto now = boost::posix_time::microsec_clock::universal_time();
            odb::transaction trans(_db->begin());
            using query = odb::query<Relation>;

            auto soft_delete = [&](const std::string &a, const std::string &b) {
                std::shared_ptr<Relation> r(_db->query_one<Relation>(
                    query::user_id == a && query::peer_id == b));
                if(r) {
                    r->status(RelationStatus::DELETED);
                    r->update_time(now);
                    _db->update(*r);
                }
            };
            soft_delete(uid, pid);
            soft_delete(pid, uid);
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("删除用户好友关系失败 {}-{}: {}", uid, pid, e.what());
            return false;
        }
        return true;
    }

    /* brief: 是否互为好友（双向都 NORMAL 才算）
     *  - 拉黑 / 删除场景下 exists 返回 false，避免给对方发消息成功
     */
    bool exists(const std::string &uid, const std::string &pid) {
        try {
            odb::transaction trans(_db->begin());
            using query = odb::query<Relation>;
            std::shared_ptr<Relation> a(_db->query_one<Relation>(
                query::user_id == uid && query::peer_id == pid &&
                query::status == RelationStatus::NORMAL));
            std::shared_ptr<Relation> b(_db->query_one<Relation>(
                query::user_id == pid && query::peer_id == uid &&
                query::status == RelationStatus::NORMAL));
            trans.commit();
            return a && b;
        } catch(std::exception &e) {
            LOG_ERROR("查询用户好友关系失败 {}-{}: {}", uid, pid, e.what());
            return false;
        }
    }

    /* brief: 获取我的所有正常好友 ID（status=NORMAL）
     *  - 拉黑/已删除不返回；走 uk_user_peer 前缀扫描，不需要回表
     */
    std::vector<std::string> friends(const std::string &uid) {
        std::vector<std::string> res;
        try {
            odb::transaction trans(_db->begin());
            using query  = odb::query<Relation>;
            using result = odb::result<Relation>;
            result r(_db->query<Relation>(
                query::user_id == uid && query::status == RelationStatus::NORMAL));
            for(auto &row : r) res.push_back(row.peer_id());
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("通过用户ID: {} 获取好友ID失败: {}", uid, e.what());
        }
        return res;
    }

    /* brief: 获取关系详情（含备注/分组/状态等） */
    std::shared_ptr<Relation> select(const std::string &uid, const std::string &pid) {
        std::shared_ptr<Relation> res;
        try {
            odb::transaction trans(_db->begin());
            using query = odb::query<Relation>;
            res.reset(_db->query_one<Relation>(
                query::user_id == uid && query::peer_id == pid));
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("查询好友关系详情失败 {}-{}: {}", uid, pid, e.what());
        }
        return res;
    }

    /* brief: 修改对对端的备注 / 分组 / 星标 — 仅影响"我"这一行 */
    bool update_meta(const std::string &uid, const std::string &pid,
                     const std::string &remark,
                     const std::string &group_name,
                     bool starred)
    {
        try {
            odb::transaction trans(_db->begin());
            using query = odb::query<Relation>;
            std::shared_ptr<Relation> r(_db->query_one<Relation>(
                query::user_id == uid && query::peer_id == pid));
            if(!r) {
                trans.commit();
                return false;
            }
            r->remark(remark);
            r->group_name(group_name);
            r->starred(starred);
            r->update_time(boost::posix_time::microsec_clock::universal_time());
            _db->update(*r);
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("更新好友备注失败 {}-{}: {}", uid, pid, e.what());
            return false;
        }
        return true;
    }

    /* brief: 拉黑 / 解除拉黑 — 仅影响"我"这一行
     *  - 与 remove() 区别：BLOCKED 仍保留好友关系，对端可见仍是好友；
     *    DELETED 则彻底视为不是好友
     */
    bool set_status(const std::string &uid, const std::string &pid, RelationStatus status) {
        try {
            odb::transaction trans(_db->begin());
            using query = odb::query<Relation>;
            std::shared_ptr<Relation> r(_db->query_one<Relation>(
                query::user_id == uid && query::peer_id == pid));
            if(!r) {
                trans.commit();
                return false;
            }
            r->status(status);
            r->update_time(boost::posix_time::microsec_clock::universal_time());
            _db->update(*r);
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("修改好友关系状态失败 {}-{}: {}", uid, pid, e.what());
            return false;
        }
        return true;
    }

private:
    std::shared_ptr<odb::core::database> _db;
};

} // namespace chatnow
