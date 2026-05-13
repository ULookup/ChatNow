#pragma once

#include "logger.hpp"
#include "mysql.hpp"
#include "friend_apply.hxx"
#include "friend_apply-odb.hxx"
#include <odb/database.hxx>
#include <odb/mysql/database.hxx>

#include <vector>
#include <string>
#include <memory>

namespace chatnow
{

/**
 * FriendApplyTable
 * ------------------------------------------------------------------
 * friend_apply 表的 DAO 封装。
 *
 * 关键变化：
 *   - select_pending：只取 status=PENDING（用 status 枚举常量代替魔法数字）
 *   - select_recent_apply：防骚扰判重接口，查"我对对方在 N 分钟内是否已申请过"
 *     —— 走新设计的 idx_user_peer_time 索引
 *   - update_status：状态机转换接口，自动写 handle_time
 *   - select_by_event_id：基于事件 ID 的通知回调入口
 *   - 删除老的 remove(uid, pid)：申请记录不应物理删除（审计追溯）
 *     —— 撤回申请走 update_status(CANCELED)
 * ------------------------------------------------------------------
 */
class FriendApplyTable
{
public:
    using ptr = std::shared_ptr<FriendApplyTable>;
    FriendApplyTable(const std::shared_ptr<odb::core::database> &db) : _db(db) {}

    /* brief: 新增好友申请记录（自动写 create_time） */
    bool insert(FriendApply &ev) {
        try {
            ev.create_time(boost::posix_time::microsec_clock::universal_time());
            odb::transaction trans(_db->begin());
            _db->persist(ev);
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("新增好友申请失败 {}-{}: {}", ev.user_id(), ev.peer_id(), e.what());
            return false;
        }
        return true;
    }

    /* brief: 申请方对同一对端是否有 PENDING 中的申请
     *  - 防止用户重复点击产生多条 PENDING；走 idx_user_peer_time 索引
     */
    bool exists_pending(const std::string &uid, const std::string &pid) {
        try {
            odb::transaction trans(_db->begin());
            using query = odb::query<FriendApply>;
            std::shared_ptr<FriendApply> r(_db->query_one<FriendApply>(
                query::user_id == uid &&
                query::peer_id == pid &&
                query::status == FriendApplyStatus::PENDING));
            trans.commit();
            return r != nullptr;
        } catch(std::exception &e) {
            LOG_ERROR("查询 PENDING 申请失败 {}-{}: {}", uid, pid, e.what());
            return false;
        }
    }

    /* brief: 通用更新（同意/拒绝/撤回时附带 handle_time） */
    bool update(const std::shared_ptr<FriendApply> &fa) {
        try {
            odb::transaction trans(_db->begin());
            _db->update(*fa);
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("更新申请失败: {}-{}: {}", fa->user_id(), fa->peer_id(), e.what());
            return false;
        }
        return true;
    }

    /* brief: 状态机转换：原子更新状态 + 处理时间
     *  - 强制业务路径走此接口，避免遗漏 handle_time
     */
    bool update_status(const std::string &event_id, FriendApplyStatus new_status) {
        try {
            odb::transaction trans(_db->begin());
            using query = odb::query<FriendApply>;
            std::shared_ptr<FriendApply> r(_db->query_one<FriendApply>(
                query::event_id == event_id));
            if(!r) {
                trans.commit();
                return false;
            }
            r->status(new_status);
            r->handle_time(boost::posix_time::microsec_clock::universal_time());
            _db->update(*r);
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("更新申请状态失败 event={}: {}", event_id, e.what());
            return false;
        }
        return true;
    }

    /* brief: 我收到的待处理申请列表（按 idx_peer_status_time 倒序） */
    std::vector<FriendApply> select_pending(const std::string &uid) {
        std::vector<FriendApply> res;
        try {
            odb::transaction trans(_db->begin());
            using query  = odb::query<FriendApply>;
            using result = odb::result<FriendApply>;

            result r(_db->query<FriendApply>(
                (query::peer_id == uid &&
                 query::status == FriendApplyStatus::PENDING) +
                " ORDER BY create_time DESC"));

            for(auto &row : r) res.push_back(row);
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("获取待处理申请列表失败 {}: {}", uid, e.what());
        }
        return res;
    }

    /* brief: 通过事件 ID 取出申请详情 — 处理回调用 */
    std::shared_ptr<FriendApply> select_by_event_id(const std::string &event_id) {
        std::shared_ptr<FriendApply> res;
        try {
            odb::transaction trans(_db->begin());
            using query = odb::query<FriendApply>;
            res.reset(_db->query_one<FriendApply>(query::event_id == event_id));
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("通过 event_id 查询申请失败 {}: {}", event_id, e.what());
        }
        return res;
    }

    /* brief: 取一对用户的最近一条申请（用于产品策略，如"X 分钟内不许再申请"） */
    std::shared_ptr<FriendApply> select_latest(const std::string &uid, const std::string &pid) {
        std::shared_ptr<FriendApply> res;
        try {
            odb::transaction trans(_db->begin());
            using query  = odb::query<FriendApply>;
            using result = odb::result<FriendApply>;
            result r(_db->query<FriendApply>(
                (query::user_id == uid && query::peer_id == pid) +
                " ORDER BY create_time DESC LIMIT 1"));
            auto it = r.begin();
            if(it != r.end()) res.reset(new FriendApply(*it));
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("查询最近申请失败 {}-{}: {}", uid, pid, e.what());
        }
        return res;
    }

private:
    std::shared_ptr<odb::core::database> _db;
};

} // namespace chatnow
