#pragma once

#include "infra/logger.hpp"
#include "dao/mysql.hpp"
#include "message_attachment.hxx"
#include "message_attachment-odb.hxx"
#include <odb/database.hxx>
#include <odb/mysql/database.hxx>

#include <memory>
#include <string>
#include <vector>

namespace chatnow
{

/**
 * MessageAttachmentTable
 * ------------------------------------------------------------------
 * message_attachment 表的 DAO 封装（多附件场景）。
 *
 * 写入约定（与 message 主表关系）：
 *   - 多附件场景（多图、视频带封面等）只写本表，message.file_id 留空
 *   - 单附件场景仍可双写以保持兼容；读取一律以本表 count > 0 为准
 *
 * 典型路径：
 *   - 上传完成后批量 insert（一条 message 对应 N 行）
 *   - 客户端拉历史消息时按 message_id 取附件清单
 * ------------------------------------------------------------------
 */
class MessageAttachmentTable
{
public:
    using ptr = std::shared_ptr<MessageAttachmentTable>;
    MessageAttachmentTable(const std::shared_ptr<odb::core::database> &db) : _db(db) {}

    /* brief: 单条插入；事务感知 — 与 message 主表是同一写事务 */
    bool insert(MessageAttachment &att) {
        try {
            if(att.create_time().is_not_a_date_time()) {
                att.create_time(boost::posix_time::microsec_clock::universal_time());
            }
            bool has_external_trans = odb::transaction::has_current();
            std::unique_ptr<odb::transaction> local_trans;
            if(!has_external_trans) {
                local_trans.reset(new odb::transaction(_db->begin()));
            }
            _db->persist(att);
            if(!has_external_trans) local_trans->commit();
        } catch(std::exception &e) {
            LOG_ERROR("插入附件失败 mid={}: {}", att.message_id(), e.what());
            throw;
        }
        return true;
    }

    /* brief: 批量插入（多图发送主路径） */
    bool insert(std::vector<MessageAttachment> &atts) {
        if(atts.empty()) return true;
        try {
            auto now = boost::posix_time::microsec_clock::universal_time();
            bool has_external_trans = odb::transaction::has_current();
            std::unique_ptr<odb::transaction> local_trans;
            if(!has_external_trans) {
                local_trans.reset(new odb::transaction(_db->begin()));
            }
            for(auto &att : atts) {
                if(att.create_time().is_not_a_date_time()) att.create_time(now);
                _db->persist(att);
            }
            if(!has_external_trans) local_trans->commit();
        } catch(std::exception &e) {
            LOG_ERROR("批量插入附件失败 count={}: {}", atts.size(), e.what());
            throw;
        }
        return true;
    }

    /* brief: 取一条消息的附件清单（按 order_idx 升序） */
    std::vector<MessageAttachment> list_of(unsigned long message_id) {
        std::vector<MessageAttachment> res;
        try {
            odb::transaction trans(_db->begin());
            using query  = odb::query<MessageAttachment>;
            using result = odb::result<MessageAttachment>;
            result r(_db->query<MessageAttachment>(
                (query::message_id == message_id) + " ORDER BY order_idx ASC"));
            for(auto &att : r) res.push_back(att);
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("查询附件清单失败 mid={}: {}", message_id, e.what());
        }
        return res;
    }

    /* brief: 批量按 message_id 取附件清单 — 拉历史消息时一次性聚合
     *  - 返回 map<message_id, attachments>，调用方按 mid 重组
     */
    std::vector<MessageAttachment> list_of_messages(const std::vector<unsigned long> &mids) {
        std::vector<MessageAttachment> res;
        if(mids.empty()) return res;
        try {
            odb::transaction trans(_db->begin());
            using query  = odb::query<MessageAttachment>;
            using result = odb::result<MessageAttachment>;
            result r(_db->query<MessageAttachment>(
                query::message_id.in_range(mids.begin(), mids.end()) +
                " ORDER BY message_id ASC, order_idx ASC"));
            for(auto &att : r) res.push_back(att);
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("批量查询附件失败: {}", e.what());
        }
        return res;
    }

    /* brief: 删除消息的所有附件（消息撤回 / 风控删除时联动） */
    bool remove_by_message(unsigned long message_id) {
        try {
            odb::transaction trans(_db->begin());
            using query = odb::query<MessageAttachment>;
            _db->erase_query<MessageAttachment>(query::message_id == message_id);
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("删除附件失败 mid={}: {}", message_id, e.what());
            return false;
        }
        return true;
    }

private:
    std::shared_ptr<odb::core::database> _db;
};

} // namespace chatnow
