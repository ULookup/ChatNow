#pragma once

#include <cstdint>
#include <chrono>
#include <mutex>
#include <stdexcept>
#include <thread>

/**
 * @brief Snowflake 风格的分布式唯一 ID 生成器
 *
 * 设计保证：
 * 1. 单实例内 ID 严格单调递增
 * 2. 多实例通过 worker_id 保证全局唯一
 * 3. 同一毫秒内使用 sequence 消除冲突
 * 4. sequence 溢出时阻塞等待下一毫秒
 * 5. 对时钟回拨提供明确处理策略
 *
 * 注意：
 * - “全局严格单调”在无中心的分布式系统中不可实现
 * - 本实现保证：单实例严格单调 + 全局趋势递增
 */

namespace chatnow
{

class SnowflakeId {
public:
    /**
     * 位宽定义（经典 Twitter Snowflake）
     *
     * 10 bits worker_id  -> 支持 1024 个节点
     * 12 bits sequence   -> 单节点单毫秒最多 4096 个 ID
     */
    static constexpr uint8_t  kWorkerBits  = 10;
    static constexpr uint8_t  kSeqBits     = 12;

    /**
     * worker_id 与 sequence 的最大值
     * 用于边界校验与溢出判断
     */
    static constexpr uint64_t kMaxWorkerId = (1ULL << kWorkerBits) - 1;
    static constexpr uint64_t kMaxSequence = (1ULL << kSeqBits) - 1;

    /**
     * @param worker_id
     *        当前生成器的机器 / 实例 ID
     *        在多实例部署时必须保证全局唯一
     *
     * @param epoch_ms
     *        自定义起始时间（毫秒）
     *        实际存储的是 (当前时间 - epoch)
     *        这样可以节省 timestamp 位数
     *
     * @param wait_on_clock_backwards
     *        是否在检测到系统时钟回拨时等待
     *        true  -> 等待时间追上 last_ts_
     *        false -> 直接抛异常
     */
    explicit SnowflakeId(uint64_t worker_id,
                         uint64_t epoch_ms = 1735689600000ULL, // 2025-01-01 UTC
                         bool wait_on_clock_backwards = true)
        : worker_id_(worker_id),
          epoch_ms_(epoch_ms),
          wait_on_clock_backwards_(wait_on_clock_backwards) {

        // worker_id 超出位宽范围，属于配置错误，直接拒绝启动
        if (worker_id_ > kMaxWorkerId) {
            throw std::invalid_argument("worker_id out of range");
        }
    }

    /**
     * @brief 生成下一个 ID
     *
     * 线程安全（mutex 串行化）：
     * - 保证 last_ts_ / sequence_ 的一致性
     * - 保证单实例内严格单调递增
     */
    uint64_t Next() {
        std::lock_guard<std::mutex> lock(mu_);

        uint64_t now = NowMs();

        /**
         * 1 时钟回拨处理
         *
         * 如果系统时间小于上一次生成 ID 的时间，
         * 说明发生了 NTP 校时或人为改时间。
         */
        if (now < last_ts_) {
            if (wait_on_clock_backwards_) {
                // 等待直到时间追平 last_ts_
                now = WaitUntil(last_ts_);
            } else {
                // 直接失败，避免生成重复 ID
                throw std::runtime_error("clock moved backwards");
            }
        }

        /**
         * 2 同一毫秒内
         *
         * 时间戳不变，只能依赖 sequence 来区分 ID
         */
        if (now == last_ts_) {
            // sequence 自增，并限制在 bit 范围内
            sequence_ = (sequence_ + 1) & kMaxSequence;

            /**
             * sequence 用尽：
             * - 当前毫秒内已经生成了 4096 个 ID
             * - 必须等待下一毫秒，否则会冲突
             */
            if (sequence_ == 0) {
                now = WaitUntil(last_ts_ + 1);
            }
        } else {
            /**
             * 3 新的毫秒
             *
             * sequence 必须清零，
             * 否则会破坏 ID 的单调性
             */
            sequence_ = 0;
        }

        // 更新最近一次生成 ID 的时间戳
        last_ts_ = now;

        // 4 拼接最终的 64-bit ID
        return Compose(now, worker_id_, sequence_);
    }

private:
    /**
     * @brief 将时间戳、worker_id、sequence 组合成最终 ID
     *
     * 结构：
     * | timestamp | worker_id | sequence |
     */
    uint64_t Compose(uint64_t ts_ms,
                     uint64_t worker,
                     uint64_t seq) const {
        // 使用相对 epoch 的时间戳，减少位宽占用
        uint64_t delta = ts_ms - epoch_ms_;

        return (delta << (kWorkerBits + kSeqBits)) |
               (worker << kSeqBits) |
               seq;
    }

    /**
     * @brief 获取当前系统时间（毫秒）
     */
    static uint64_t NowMs() {
        using namespace std::chrono;
        return duration_cast<milliseconds>(
                   system_clock::now().time_since_epoch())
            .count();
    }

    /**
     * @brief 阻塞等待直到系统时间 >= target_ms
     *
     * 设计说明：
     * - 直接 busy-spin 会浪费 CPU
     * - sleep 太粗会影响延迟
     * - 这里采用 sleep + yield 折中
     */
    static uint64_t WaitUntil(uint64_t target_ms) {
        for (;;) {
            uint64_t now = NowMs();
            if (now >= target_ms) {
                return now;
            }

            if (target_ms - now > 1) {
                // 时间差较大，sleep 1ms
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(1));
            } else {
                // 即将到达目标时间，主动让出 CPU
                std::this_thread::yield();
            }
        }
    }

private:
    // ----------- 配置参数 -----------
    uint64_t worker_id_;                 // 实例 / 机器 ID
    uint64_t epoch_ms_;                  // 起始时间
    bool     wait_on_clock_backwards_;   // 时钟回拨策略

    // ----------- 运行时状态 -----------
    std::mutex mu_;                      // 保证线程安全
    uint64_t   last_ts_  = 0;            // 上一次生成 ID 的时间
    uint64_t   sequence_ = 0;            // 当前毫秒内的序列号
};  // class SnowflakeId

} // namespace chatnow
