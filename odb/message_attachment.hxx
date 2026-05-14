#pragma once

#include <string>
#include <cstddef>
#include <odb/nullable.hxx>
#include <odb/core.hxx>
#include <boost/date_time/posix_time/posix_time.hpp>

/**
 * ===========================================================================
 * 消息附件表 (message_attachment)
 * ---------------------------------------------------------------------------
 * 设计定位：
 *   - 复杂附件场景（多图/视频带封面/文件预览图）的标准存储位
 *   - 一条消息可对应多个附件（相册多图发送），通过 message_id 关联
 *   - 媒体专属字段（宽高/时长/缩略图）固定列存放，避免散落 JSON 影响检索
 *
 * 与 message 主表的字段重复策略（重要）：
 *   - 历史保留：message.{file_id, file_name, file_size} 仍可写
 *   - 新功能写入约定：
 *       · 单文件场景（文件消息 / 单图）：可双写，主表为权威；
 *       · 多附件场景（相册/视频带封面）：只写本表，主表 file_id 留空
 *   - 读取约定：count(*) 本表 > 0 时一律以本表为准
 *   - 长期演进：等业务全部切换后，message 主表的 file_* 字段下线
 *
 * 字段速览：
 *   _id            物理主键
 *   _message_id    关联消息 ID（全局 message_id）
 *   _order_idx     在该消息内的展示顺序（多图 0/1/2/...）
 *   _att_type      附件类型：IMAGE/VIDEO/AUDIO/SPEECH/FILE/STICKER
 *   _file_id       附件文件 ID（指向 file 服务）
 *   _file_name     文件名（客户端展示与下载默认命名）
 *   _file_size     文件大小（字节）
 *   _mime_type     MIME 类型（"image/jpeg"），客户端按此判定预览方式
 *   _thumb_file_id 缩略图文件 ID（图片/视频专用）
 *   _width         媒体宽（图片/视频）
 *   _height        媒体高
 *   _duration_ms   时长毫秒（视频/音频/语音）
 *   _asr_text      语音识别文本（语音消息专用），便于消息搜索命中
 *   _create_time   创建时间（归档/清理/排查用）
 *
 * 索引策略：
 *   idx_message    (message_id, order_idx) — 按消息取附件列表，主路径
 *   说明：file_id 不单列索引，附件去查 file 服务自身的存储入口即可
 * ===========================================================================
 */

namespace chatnow
{

enum class AttachmentType : unsigned char {
    UNKNOWN = 0,
    IMAGE   = 1,    // 图片（含缩略图）
    VIDEO   = 2,    // 视频（带封面）
    AUDIO   = 3,    // 通用音频
    SPEECH  = 4,    // 语音消息（含识别文本）
    FILE    = 5,    // 文档 / 通用文件
    STICKER = 6     // 表情 / 贴纸
};

#pragma db object table("message_attachment")
class MessageAttachment
{
public:
    MessageAttachment() = default;

    unsigned long message_id() const { return _message_id; }
    void message_id(unsigned long v) { _message_id = v; }

    int order_idx() const { return _order_idx; }
    void order_idx(int v) { _order_idx = v; }

    AttachmentType att_type() const { return _att_type; }
    void att_type(AttachmentType v) { _att_type = v; }

    std::string file_id() const { return _file_id; }
    void file_id(const std::string &v) { _file_id = v; }

    std::string file_name() const { return _file_name ? *_file_name : std::string(); }
    void file_name(const std::string &v) { _file_name = v; }

    unsigned long long file_size() const { return _file_size; }
    void file_size(unsigned long long v) { _file_size = v; }

    std::string mime_type() const { return _mime_type ? *_mime_type : std::string(); }
    void mime_type(const std::string &v) { _mime_type = v; }

    std::string thumb_file_id() const { return _thumb_file_id ? *_thumb_file_id : std::string(); }
    void thumb_file_id(const std::string &v) { _thumb_file_id = v; }

    int width() const { return _width ? *_width : 0; }
    void width(int v) { _width = v; }

    int height() const { return _height ? *_height : 0; }
    void height(int v) { _height = v; }

    int duration_ms() const { return _duration_ms ? *_duration_ms : 0; }
    void duration_ms(int v) { _duration_ms = v; }

    std::string asr_text() const { return _asr_text ? *_asr_text : std::string(); }
    void asr_text(const std::string &v) { _asr_text = v; }

    boost::posix_time::ptime create_time() const { return _create_time; }
    void create_time(const boost::posix_time::ptime &v) { _create_time = v; }

private:
    friend class odb::access;

    #pragma db id auto
    unsigned long _id;

    #pragma db type("bigint unsigned")
    unsigned long _message_id;

    #pragma db type("int")
    int _order_idx {0};

    #pragma db type("tinyint unsigned")
    AttachmentType _att_type {AttachmentType::UNKNOWN};

    #pragma db type("varchar(64)")
    std::string _file_id;

    #pragma db type("varchar(255)")
    odb::nullable<std::string> _file_name;

    #pragma db type("bigint unsigned")
    unsigned long long _file_size {0};

    #pragma db type("varchar(64)")
    odb::nullable<std::string> _mime_type;

    #pragma db type("varchar(64)")
    odb::nullable<std::string> _thumb_file_id;

    #pragma db type("int unsigned")
    odb::nullable<int> _width;

    #pragma db type("int unsigned")
    odb::nullable<int> _height;

    #pragma db type("int unsigned")
    odb::nullable<int> _duration_ms;

    #pragma db type("text")
    odb::nullable<std::string> _asr_text;

    // 附件创建时间：归档清理 / 故障排查必需
    #pragma db type("DATETIME(3)")
    boost::posix_time::ptime _create_time;

    #pragma db index("idx_message") members(_message_id, _order_idx)
};

} // namespace chatnow
