#pragma once

#include <string>
#include <cstddef>
#include <odb/nullable.hxx>
#include <odb/core.hxx>

/**
 * ===========================================================================
 * 消息 @ 提及表 (message_mention)
 * ---------------------------------------------------------------------------
 * 设计定位：
 *   - 取代 message._mention_user_ids 那种 "varchar 逗号分隔" 反范式存储
 *   - 让 "查询某用户是否被 @"、"我所有被 @ 的消息列表" 走索引而非扫描
 *
 * 工作模式：
 *   1. 普通点名 @：每被 @ 一人写一行
 *   2. 全体提及（@all）：单写一行，user_id = "*" 即可，避免群人数膨胀写放大
 *      - 客户端拉取被 @ 列表时，需同时匹配自己的 user_id 与通配 "*"
 *      - 如果群成员无敏感隐私需求，也可用 "@all" 字面值替代
 *
 * 字段速览：
 *   _id          物理主键
 *   _message_id  关联消息 ID（全局 message_id）
 *   _user_id     被 @ 用户 ID；"*" 表示全体提及
 *
 * 索引策略：
 *   uk_msg_user  (message_id, user_id) UNIQUE — 同一消息对同一用户只 @ 一次
 *   idx_user_msg (user_id, message_id)        — "查我被 @ 的所有消息"
 * ===========================================================================
 */

namespace chatnow
{

#pragma db object table("message_mention")
class MessageMention
{
public:
    MessageMention() = default;
    MessageMention(unsigned long mid, const std::string &uid)
        : _message_id(mid), _user_id(uid) {}

    unsigned long message_id() const { return _message_id; }
    void message_id(unsigned long v) { _message_id = v; }

    std::string user_id() const { return _user_id; }
    void user_id(const std::string &v) { _user_id = v; }

private:
    friend class odb::access;

    #pragma db id auto
    unsigned long _id;

    #pragma db type("bigint unsigned")
    unsigned long _message_id;

    // 被 @ 用户 ID；约定 "*" 表示全体提及（@all）
    #pragma db type("varchar(32)")
    std::string _user_id;

    #pragma db index("uk_msg_user") unique members(_message_id, _user_id)
    #pragma db index("idx_user_msg") members(_user_id, _message_id)
};

} // namespace chatnow
