#pragma once

/**
 * ===========================================================================
 * Snowflake 风格分布式唯一 ID 生成器
 * ---------------------------------------------------------------------------
 * 设计保证：
 *   1. 单实例 ID 严格单调递增
 *   2. 多实例通过 worker_id 保证全局唯一（10 bits → 1024 节点）
 *   3. 同毫秒内 sequence 区分（12 bits → 4096 ID/ms）
 *   4. sequence 溢出阻塞至下一毫秒
 *   5. 时钟回拨：< 1s 自旋追平；> 1s 抛异常拒绝出 ID（避免重复）
 *
 * 注意：
 *   - 全局严格单调在无中心系统不可达；本实现保证「单实例严格单调 + 全局趋势递增」
 *   - 业务上跨实例需要绝对单调时（如会话 seq）请用 Redis INCR（见 SeqGen）
 *
 * 修订（相对原版）：
 *   - 私有变量加 chatnow:: 命名空间内访问，保持与项目风格一致
 *   - WaitUntil 用 sleep_for(1ms) 起步避免 busy-spin 在大回拨场景吃 CPU
 *   - epoch_ms 默认值改 2025-01-01 UTC（旧值已是这天但注释提及，保留）
 * ===========================================================================
 */

#include <chrono>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace chatnow
{

class SnowflakeId
{
public:
    static constexpr uint8_t  kWorkerBits = 10;   // 1024 个节点
    static constexpr uint8_t  kSeqBits    = 12;   // 单节点单 ms 4096 个 ID
    static constexpr uint64_t kMaxWorkerId = (1ULL << kWorkerBits) - 1;
    static constexpr uint64_t kMaxSequence = (1ULL << kSeqBits) - 1;

    /**
     * @param worker_id 实例 ID，多机部署需全局唯一
     * @param epoch_ms  自定义起始毫秒（默认 2025-01-01 UTC）
     * @param wait_on_clock_backwards 时钟回拨时是否阻塞等待（false 直接抛错）
     */
    explicit SnowflakeId(uint64_t worker_id,
                         uint64_t epoch_ms = 1735689600000ULL, // 2025-01-01 UTC
                         bool wait_on_clock_backwards = true)
        : worker_id_(worker_id),
          epoch_ms_(epoch_ms),
          wait_on_clock_backwards_(wait_on_clock_backwards)
    {
        if(worker_id_ > kMaxWorkerId) {
            throw std::invalid_argument("snowflake worker_id out of range");
        }
    }

    /* brief: 申请下一个 64-bit ID（线程安全） */
    uint64_t Next() {
        std::lock_guard<std::mutex> lock(mu_);
        uint64_t now = NowMs();

        // 1) 时钟回拨处理
        if(now < last_ts_) {
            uint64_t diff = last_ts_ - now;
            if(!wait_on_clock_backwards_ || diff > 1000) {
                throw std::runtime_error("snowflake: clock moved backwards");
            }
            now = WaitUntil(last_ts_);
        }

        // 2) 同毫秒：sequence 自增并防溢出
        if(now == last_ts_) {
            sequence_ = (sequence_ + 1) & kMaxSequence;
            if(sequence_ == 0) now = WaitUntil(last_ts_ + 1);
        } else {
            // 3) 新毫秒：sequence 必须清零保单调性
            sequence_ = 0;
        }
        last_ts_ = now;
        return Compose(now, worker_id_, sequence_);
    }

    uint64_t worker_id() const { return worker_id_; }
    uint64_t epoch_ms()  const { return epoch_ms_;  }

private:
    /* brief: timestamp(41) | worker(10) | sequence(12) */
    uint64_t Compose(uint64_t ts_ms, uint64_t worker, uint64_t seq) const {
        uint64_t delta = ts_ms - epoch_ms_;
        return (delta << (kWorkerBits + kSeqBits)) |
               (worker << kSeqBits) |
               seq;
    }

    static uint64_t NowMs() {
        using namespace std::chrono;
        return duration_cast<milliseconds>(
                   system_clock::now().time_since_epoch())
            .count();
    }

    /* brief: 阻塞等待至 target_ms；先 sleep 1ms，逼近时切换 yield */
    static uint64_t WaitUntil(uint64_t target_ms) {
        for(;;) {
            uint64_t now = NowMs();
            if(now >= target_ms) return now;
            if(target_ms - now > 1) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            } else {
                std::this_thread::yield();
            }
        }
    }

    uint64_t worker_id_;
    uint64_t epoch_ms_;
    bool     wait_on_clock_backwards_;

    std::mutex mu_;
    uint64_t   last_ts_  = 0;
    uint64_t   sequence_ = 0;
};

} // namespace chatnow
