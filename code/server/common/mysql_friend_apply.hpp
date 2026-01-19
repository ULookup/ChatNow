#pragma once

#include "logger.hpp"
#include "mysql.hpp"
#include "friend_apply.hxx"
#include "friend_apply-odb.hxx"
#include <odb/database.hxx>
#include <odb/mysql/database.hxx>

namespace chatnow 
{

class FriendApplyTable
{
public:
    using ptr = std::shared_ptr<FriendApplyTable>;
    FriendApplyTable(const std::shared_ptr<odb::core::database> &db) : _db(db) {}

    /* brief: 新增好友申请信息 */
    bool insert(FriendApply &ev) {
        try {
            odb::transaction trans(_db->begin());

            _db->persist(ev);

            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("新增好友申请失败 {}-{}: {}", ev.user_id(), ev.peer_id(), e.what());
            return false;
        }
        return true;
    }
    /* brief: 判断是否已存在申请 */
    bool exists(const std::string &uid, const std::string &pid) {
        typedef odb::query<FriendApply> query;
        typedef odb::result<FriendApply> result;
        result r;
        bool flag = false;
        try {
            odb::transaction trans(_db->begin());

            r = _db->query<FriendApply>(query::user_id == uid && query::peer_id == pid);
            flag = !r.empty();
            
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("获取好友申请事件失败: {} - {}: {}", uid, pid, e.what());
        }
        return flag;
    }
    /* brief: 删除好友申请 */
    bool remove(const std::string &uid, const std::string &pid) {
        try {
            odb::transaction trans(_db->begin());
            
            typedef odb::query<FriendApply> query;
            typedef odb::result<FriendApply> result;
            _db->erase_query<FriendApply>(query::user_id == uid && query::peer_id == pid);

            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("删除好友申请事件失败 {}-{}: {}", uid, pid, e.what());
            return false;
        }
        return true;
    }
    /* brief: 获取好友申请(可以被 select 完全替代) */
    std::vector<std::string> apply_users(const std::string &uid) {
        std::vector<std::string> res;
        try {
            odb::transaction trans(_db->begin());
            
            typedef odb::query<FriendApply> query;
            typedef odb::result<FriendApply> result;
            // 当前的 uid 是被申请者的用户ID
            result r(_db->query<FriendApply>(query::peer_id == uid));
            for(result::iterator i(r.begin()); i != r.end(); ++i) {
                res.push_back(i->user_id());
            }
 
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("通过用户ID: {} 获取所有好友申请发起者失败: {}", uid, e.what());
        }
        return res;
    }
    //====================================================================================
    //=======================================V2.0=========================================
    //====================================================================================
    /* brief: 更新申请状态 */
    bool update(const std::shared_ptr<FriendApply> &friend_apply) {
        try {
            odb::transaction trans(_db->begin()); // 获取事务对象，开启事务

            _db->update(*friend_apply);

            trans.commit(); // 提交事务
        } catch(std::exception &e) {
            LOG_ERROR("更新好友申请状态失败: {}-{}:{}", friend_apply->user_id(), friend_apply->peer_id(), e.what());
            return false;
        }
        return true;
    }
    /* brief: 获取申请信息 */
    std::vector<FriendApply> select(const std::string &uid) {
        std::vector<FriendApply> res;
        try {
            odb::transaction trans(_db->begin()); // 获取事务对象，开启事务

            using query  = odb::query<FriendApply>;
            using result = odb::result<FriendApply>;

            result r(_db->query<FriendApply>((query::peer_id == uid) && (query::status == chatnow::FriendApplyStatus::PENDING)));

            for(result::iterator i(r.begin()); i != r.end(); ++i) {
                res.push_back(*i);
            }

            trans.commit(); // 提交事务
        } catch(std::exception &e) {
            LOG_ERROR("获取申请信息失败 {}:{}", uid, e.what());
        }
        return res;
    }
    /* brief: 获取指定申请信息 */
    std::shared_ptr<FriendApply> select(const std::string &uid, const std::string &pid) {
        std::shared_ptr<FriendApply> res;
        try {
            odb::transaction trans(_db->begin()); // 获取事务对象，开启事务

            using query  = odb::query<FriendApply>;
            using result = odb::result<FriendApply>;

            res.reset(_db->query_one<FriendApply>((query::user_id == uid) && (query::peer_id == pid)));

            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("通过申请者被申请者查询申请信息失败 {}-{}:{}", uid, pid, e.what());
        }
        return res;
    }
private:
    std::shared_ptr<odb::core::database> _db;
};

}