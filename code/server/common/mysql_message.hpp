#pragma once

#include "logger.hpp"
#include "mysql.hpp"
#include "message.hxx"
#include "message-odb.hxx"

namespace chatnow
{

class MessageTable
{
public:
    using ptr = std::shared_ptr<MessageTable>;
    MessageTable(const std::shared_ptr<odb::core::database> &db) : _db(db) {}
    ~MessageTable() {}
    bool insert(Message &msg) {
        try {
            // 1. 检查外部是否已经开启了事务
            bool has_external_trans = odb::transaction::has_current();

            // 2. 如果外部没有事务，我们才开启一个本地事务；否则只创建一个空的 holder
            // std::unique_ptr 用于管理事务对象的生命周期
            std::unique_ptr<odb::transaction> local_trans;
            
            if (!has_external_trans) {
                // 只有在没事务的时候，才由 DAO 开启
                local_trans.reset(new odb::transaction(_db->begin()));
            }

            // 3. 执行持久化操作 (ODB 会自动挂载到当前线程的活跃事务上)
            _db->persist(msg);

            // 4. 只有当我们自己开启了事务，我们才负责提交
            if (!has_external_trans) {
                local_trans->commit();
            }
            
            // 如果是外部事务，这里什么都不做，等外部 Service 去 commit
        } catch(std::exception &e) {
            LOG_ERROR("插入失败: {}", e.what());
            throw; // 异常会向上传递，导致外部事务回滚
        }
        return true;
    }
    bool remove(const std::string &ssid) {
        try {
            odb::transaction trans(_db->begin());
            typedef odb::query<Message> query;
            typedef odb::result<Message> result;
            _db->erase_query<Message>(query::session_id == ssid);
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("删除会话所有消息失败 {} : {}", ssid, e.what());
            return false;
        }
        return true;
    }
    std::vector<Message> recent(const std::string &ssid, int count) {
        std::vector<Message> res;
        try {
            odb::transaction trans(_db->begin());
            typedef odb::query<Message> query;
            typedef odb::result<Message> result;
            // 本次查询以ssid作为过滤条件，然后进行以时间字段进行逆序，通过limit
            // session_id='xx' order by create_time desc limit cont;
            std::stringstream cond;
            cond << "session_id='" << ssid << "' ";
            cond << "order by create_time desc limit " << count;
            result r(_db->query<Message>(cond.str()));
            for(result::iterator i(r.begin()); i != r.end(); ++i) {
                res.push_back(*i);
            }
            std::reverse(res.begin(), res.end());
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("获取最近消息失败 {}-{}-{}", ssid, count, e.what());
        }
        return res;
    }
    std::vector<Message> range(const std::string &ssid, boost::posix_time::ptime stime, boost::posix_time::ptime etime) {
        std::vector<Message> res;
        try {
            odb::transaction trans(_db->begin());
            typedef odb::query<Message> query;
            typedef odb::result<Message> result;
            // 获取指定会话指定时间段的信息
            result r(_db->query<Message>(query::session_id == ssid && query::create_time >= stime && query::create_time <= etime));
            for(result::iterator i(r.begin()); i != r.end(); ++i) {
                res.push_back(*i);
            }
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("获取区间消息失败 {}-{}:{}-{}", ssid, boost::posix_time::to_simple_string(stime), boost::posix_time::to_simple_string(etime), e.what());
        }
        return res;
    }
    // 在 MessageTable 类中增加
    std::vector<Message> select_by_ids(const std::vector<unsigned long>& ids) {
        std::vector<Message> res;
        if (ids.empty()) return res;
        
        try {
            odb::transaction trans(_db->begin());
            typedef odb::query<Message> query;
            typedef odb::result<Message> result;
            
            // 使用 IN 语法：WHERE message_id IN (1, 2, 3...)
            result r(_db->query<Message>(query::message_id.in_range(ids.begin(), ids.end()) + 
                                        " ORDER BY create_time ASC")); 
            
            for(auto& m : r) {
                res.push_back(m);
            }
            trans.commit();
        } catch(std::exception& e) {
            LOG_ERROR("批量主键查询消息失败: {}", e.what());
        }
        return res;
    }
private:
    std::shared_ptr<odb::core::database> _db;
};

}