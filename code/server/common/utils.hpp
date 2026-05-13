#pragma once

/**
 * ===========================================================================
 * 公共工具集
 * ---------------------------------------------------------------------------
 * 设计要点：
 *   1. uuid()：原子计数器改成 inline static 跨调用累计，不再每次重置
 *   2. 新增 verify_code(n)：可指定位数，默认 6 位（4 位强度过低）
 *   3. 新增 single_session_id(uid_a, uid_b)：约定双方 user_id 排序后
 *      生成确定性单聊会话 ID，客户端无需查库即可推算
 *   4. 文件 IO 使用 RAII，明确错误码记录到日志
 *   5. 新增时间转字符串、boost::ptime <=> 毫秒/秒戳互转工具，
 *      消息时间戳上下游统一格式
 *   6. 全部声明 inline 避免多 TU 重复定义
 * ===========================================================================
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <random>
#include <sstream>
#include <string>
#include <boost/date_time/posix_time/posix_time.hpp>
#include "logger.hpp"

namespace chatnow
{

/* brief: 生成一个 16 位 16 进制字符串作为唯一 ID
 *  格式: aabb-ccddee-NNNN  （6 字节随机 + 单调递增 2 字节序号）
 */
inline std::string uuid() {
    static thread_local std::mt19937 generator(std::random_device{}());
    static std::atomic<uint16_t> idx{0};
    std::uniform_int_distribution<int> distribution(0, 255);

    std::stringstream ss;
    for(int i = 0; i < 6; ++i) {
        if(i == 2) ss << "-";
        ss << std::setw(2) << std::setfill('0') << std::hex << distribution(generator);
    }
    ss << "-";
    ss << std::setw(4) << std::setfill('0') << std::hex << idx.fetch_add(1, std::memory_order_relaxed);
    return ss.str();
}

/* brief: 生成 N 位纯数字验证码（默认 6 位） */
inline std::string verify_code(int len = 6) {
    static thread_local std::mt19937 generator(std::random_device{}());
    std::uniform_int_distribution<int> distribution(0, 9);
    std::stringstream ss;
    for(int i = 0; i < len; ++i) ss << distribution(generator);
    return ss.str();
}

/* brief: 旧名兼容：4 位验证码（已不推荐，逐步替换为 verify_code(6)） */
inline std::string verifyCode() { return verify_code(4); }

/* brief: 计算单聊会话 ID
 *  - 约定：将双方 user_id 按字典序排序后拼接，前缀 single:
 *  - 客户端可不查库直接算出 chat_session_id，避免一次额外往返
 *  - 配合 chat_session.peer_user_id 字段，零 join 即可定位单聊
 */
inline std::string single_session_id(const std::string &a, const std::string &b) {
    if(a.empty() || b.empty()) return std::string();
    return a < b ? ("single:" + a + "_" + b) : ("single:" + b + "_" + a);
}

/* brief: 当前时间转 boost::ptime（毫秒精度） */
inline boost::posix_time::ptime now_pt() {
    return boost::posix_time::microsec_clock::universal_time();
}

/* brief: ptime 转毫秒时间戳（UTC） */
inline int64_t pt_to_ms(const boost::posix_time::ptime &t) {
    static const boost::posix_time::ptime epoch(boost::gregorian::date(1970, 1, 1));
    return (t - epoch).total_milliseconds();
}

/* brief: ptime 转秒级时间戳（UTC） */
inline int64_t pt_to_seconds(const boost::posix_time::ptime &t) {
    static const boost::posix_time::ptime epoch(boost::gregorian::date(1970, 1, 1));
    return (t - epoch).total_seconds();
}

/* brief: 毫秒戳转 ptime */
inline boost::posix_time::ptime ms_to_pt(int64_t ms) {
    static const boost::posix_time::ptime epoch(boost::gregorian::date(1970, 1, 1));
    return epoch + boost::posix_time::milliseconds(ms);
}

/* brief: 读取文件全部内容到 body */
inline bool readFile(const std::string &filename, std::string &body) {
    std::ifstream ifs(filename, std::ios::binary | std::ios::in);
    if(ifs.is_open() == false) {
        LOG_ERROR("打开文件 {} 失败! errno={}", filename, errno);
        return false;
    }
    ifs.seekg(0, std::ios::end);
    const auto flen = ifs.tellg();
    if(flen < 0) {
        LOG_ERROR("获取文件大小失败 {}", filename);
        return false;
    }
    ifs.seekg(0, std::ios::beg);
    body.resize(static_cast<size_t>(flen));
    ifs.read(&body[0], flen);
    if(!ifs.good() && !ifs.eof()) {
        LOG_ERROR("读取文件 {} 数据失败!", filename);
        return false;
    }
    return true;
}

/* brief: 把 body 写入文件（覆盖） */
inline bool writeFile(const std::string &filename, const std::string &body) {
    std::ofstream ofs(filename, std::ios::out | std::ios::binary | std::ios::trunc);
    if(ofs.is_open() == false) {
        LOG_ERROR("打开文件 {} 失败!", filename);
        return false;
    }
    ofs.write(body.c_str(), static_cast<std::streamsize>(body.size()));
    if(!ofs.good()) {
        LOG_ERROR("写入文件 {} 失败", filename);
        return false;
    }
    return true;
}

} // namespace chatnow
