#pragma once

#include "infra/logger.hpp"
#include "dao/mysql.hpp"
#include "conversation.hxx"
#include "conversation-odb.hxx"
#include "conversation_view.hxx"
#include "conversation_view-odb.hxx"
#include "conversation_member.hxx"
#include "conversation_member-odb.hxx"
#include <odb/database.hxx>
#include <odb/mysql/database.hxx>

#include <vector>
#include <string>
#include <memory>

namespace chatnow
{

class ConversationTable
{
public:
    using ptr = std::shared_ptr<ConversationTable>;
    ConversationTable(const std::shared_ptr<odb::core::database> &db) : _db(db) {}

    /* brief: 新增会话；自动写 create_time / update_time */
    bool insert(Conversation &c) {
        try {
            auto now = boost::posix_time::microsec_clock::universal_time();
            c.create_time(now);
            c.update_time(now);

            odb::transaction trans(_db->begin());
            _db->persist(c);
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("新增会话失败 {}: {}", c.conversation_name(), e.what());
            return false;
        }
        return true;
    }

    /* brief: 改会话 status（软删除/归档/复活均走此路）+ 刷 update_time */
    bool update_status(const std::string &cid, ConversationStatus s) {
        try {
            odb::transaction trans(_db->begin());
            using query = odb::query<Conversation>;
            std::shared_ptr<Conversation> c(_db->query_one<Conversation>(
                query::conversation_id == cid));
            if(!c) {
                trans.commit();
                return false;
            }
            c->status(s);
            c->update_time(boost::posix_time::microsec_clock::universal_time());
            _db->update(*c);
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("更新会话 status 失败 {}: {}", cid, e.what());
            return false;
        }
        return true;
    }

    /* brief: 通过会话 ID 查会话 */
    std::shared_ptr<Conversation> select(const std::string &cid) {
        std::shared_ptr<Conversation> res;
        try {
            odb::transaction trans(_db->begin());
            using query = odb::query<Conversation>;
            res.reset(_db->query_one<Conversation>(query::conversation_id == cid));
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("查询会话失败 {}: {}", cid, e.what());
        }
        return res;
    }

    /* brief: 通过对端 user_id 取单聊会话 */
    std::shared_ptr<Conversation> select_private_by_peer(const std::string &uid, const std::string &pid) {
        std::shared_ptr<Conversation> res;
        try {
            odb::transaction trans(_db->begin());
            using query = odb::query<Conversation>;
            res.reset(_db->query_one<Conversation>(
                query::conversation_type == ConversationType::PRIVATE &&
                query::peer_user_id == pid));
            (void)uid;
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("通过对端查单聊失败 {}-{}: {}", uid, pid, e.what());
        }
        return res;
    }

    bool private_exists(const std::string &uid, const std::string &pid) {
        return static_cast<bool>(select_private_by_peer(uid, pid));
    }

    bool exists(const std::string &cid) {
        try {
            odb::transaction trans(_db->begin());
            using query = odb::query<Conversation>;
            std::shared_ptr<Conversation> r(_db->query_one<Conversation>(
                query::conversation_id == cid));
            trans.commit();
            return r != nullptr;
        } catch(std::exception &e) {
            LOG_ERROR("判断会话是否存在失败 {}: {}", cid, e.what());
            return false;
        }
    }

    bool update(const std::shared_ptr<Conversation> &c) {
        try {
            c->update_time(boost::posix_time::microsec_clock::universal_time());
            odb::transaction trans(_db->begin());
            _db->update(*c);
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("更新会话失败 {}: {}", c->conversation_id(), e.what());
            return false;
        }
        return true;
    }

    bool bump_max_seq(const std::string &cid, unsigned long new_seq) {
        try {
            odb::transaction trans(_db->begin());
            using query = odb::query<Conversation>;
            std::shared_ptr<Conversation> c(_db->query_one<Conversation>(
                (query::conversation_id == cid) + " FOR UPDATE"));
            if(!c) {
                trans.commit();
                return false;
            }
            if(c->max_seq() < new_seq) {
                c->max_seq(new_seq);
                _db->update(*c);
            }
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("刷新 max_seq 失败 {}: {}", cid, e.what());
            return false;
        }
        return true;
    }

    std::vector<Conversation> select(const std::vector<std::string> &cids) {
        std::vector<Conversation> res;
        if(cids.empty()) return res;
        try {
            odb::transaction trans(_db->begin());
            using query  = odb::query<Conversation>;
            using result = odb::result<Conversation>;
            result r(_db->query<Conversation>(
                query::conversation_id.in_range(cids.begin(), cids.end())));
            for(auto &c : r) res.push_back(c);
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("批量取会话失败: {}", e.what());
        }
        return res;
    }

private:
    std::shared_ptr<odb::core::database> _db;
};

} // namespace chatnow
