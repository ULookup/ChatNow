#pragma once

/**
 * ===========================================================================
 * RabbitMQ 封装（基于 AMQP-CPP + libev）
 * ---------------------------------------------------------------------------
 * 设计要点（在原版基础上的修订）：
 *   1. 修复 DLX 命名 bug：dlx_xxx() 由 "exchange + dlx_" 改为 "dlx_ + exchange"
 *      —— 旧版会拼成 "chat_msg_exchangedlx_" 这种奇怪字符串
 *   2. delayed_ttl 单位明确为毫秒（与 AMQP x-message-ttl 一致）
 *   3. 错误处理策略放松：库内不再 abort()，改为返回失败并打日志，
 *      由调用方决定是否要 fatal exit；这是库代码该有的行为
 *   4. 新增 qos(prefetch) 限制：默认 prefetch=64，避免慢消费者内存堆积
 *   5. publish_confirm 状态枚举显式注释 Lost vs Nack 的区别
 *   6. 析构关闭 connection 后再销毁 ev_loop，避免事件残留
 * ===========================================================================
 */

#include <ev.h>
#include <amqpcpp.h>
#include <amqpcpp/libev.h>
#include <openssl/ssl.h>
#include <openssl/opensslv.h>
#include <condition_variable>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <thread>
#include "logger.hpp"

namespace chatnow
{

inline const std::string DIRECT  = "direct";
inline const std::string FANOUT  = "fanout";
inline const std::string TOPIC   = "topic";
inline const std::string HEADERS = "headers";
inline const std::string DELAYED = "delayed";
inline const std::string DLX_PREFIX = "dlx_";
inline const std::string DEAD_LETTER_EXCHANGE    = "x-dead-letter-exchange";
inline const std::string DEAD_LETTER_ROUTING_KEY = "x-dead-letter-routing-key";
inline const std::string MESSAGE_TTL             = "x-message-ttl";

inline constexpr uint16_t kDefaultPrefetch = 64;

struct declare_settings {
    std::string exchange;
    std::string exchange_type;       // direct / fanout / topic / headers / delayed
    std::string queue;
    std::string binding_key;
    /* brief: 延时消息的 TTL（毫秒），与 AMQP x-message-ttl 单位一致；旧实现注释为秒是错的 */
    int64_t delayed_ttl_ms = 5000;

    /* brief: DLX 资源命名（前缀方式，非旧版的尾部拼接） */
    std::string dlx_exchange()     const { return DLX_PREFIX + exchange; }
    std::string dlx_queue()        const { return DLX_PREFIX + queue; }
    std::string dlx_binding_key()  const { return DLX_PREFIX + binding_key; }
};

inline AMQP::ExchangeType exchange_type(const std::string &type) {
    if (type == DIRECT)  return AMQP::ExchangeType::direct;
    if (type == FANOUT)  return AMQP::ExchangeType::fanout;
    if (type == TOPIC)   return AMQP::ExchangeType::topic;
    if (type == HEADERS) return AMQP::ExchangeType::headers;
    if (type == DELAYED) return AMQP::ExchangeType::direct;
    LOG_ERROR("无效的交换机类型: {}", type);
    return AMQP::ExchangeType::direct;
}

/* brief: 发布确认状态
 *  - Acked   : broker 已确认接收
 *  - Nacked  : broker 显式拒绝（如队列满 / 路由失败）
 *  - Lost    : 与 broker 间通道断开导致状态未知（应视为可能丢失）
 *  - Error   : 发布过程发生 channel 错误
 */
enum class PublishStatus { Acked, Nacked, Lost, Error };

/* brief: 消费回调返回 — 决定本条消息的 ack 策略 */
enum class ConsumeAction { Ack, NackRequeue, NackDiscard };

using MessageCallback         = std::function<ConsumeAction(const char*, size_t, bool)>;
using PublishConfirmCallback  = std::function<void(PublishStatus, const std::string&)>;

class MQClient
{
public:
    using ptr = std::shared_ptr<MQClient>;

    explicit MQClient(const std::string &url)
        : _ev_loop(ev_loop_new(EVFLAG_AUTO))
        , _handler(_ev_loop)
        , _connection(&_handler, AMQP::Address(url))
        , _channel(&_connection)
        , _reliable(std::make_unique<AMQP::Reliable<>>(_channel))
    {
        ev_async_init(&_ev_async, quit_callback);
        ev_async_start(_ev_loop, &_ev_async);

        _task_watcher.data = this;
        ev_async_init(&_task_watcher, task_callback);
        ev_async_start(_ev_loop, &_task_watcher);

        _async_thread = std::thread([this](){ ev_run(_ev_loop, 0); });
    }

    ~MQClient() {
        // 优雅关闭：先在 ev 线程内关闭 channel/connection，再 break loop
        post_task([this](){
            try { _channel.close(); _connection.close(); } catch(...) {}
        });
        ev_async_send(_ev_loop, &_ev_async);
        if(_async_thread.joinable()) _async_thread.join();
        ev_loop_destroy(_ev_loop);
    }

    /* brief: 声明交换机 / 队列 / 绑定（含 DLX 链路） */
    void declare(const declare_settings &settings) {
        AMQP::Table args;
        if(settings.exchange_type == DELAYED) {
            // 1) 先声明 DLX 链路
            _declared(settings, AMQP::Table(), /*is_dlx=*/true);
            // 2) 主队列绑定 TTL + DLX 路由
            args[MESSAGE_TTL]              = (int64_t)settings.delayed_ttl_ms;
            args[DEAD_LETTER_EXCHANGE]     = settings.dlx_exchange();
            args[DEAD_LETTER_ROUTING_KEY]  = settings.dlx_binding_key();
        }
        _declared(settings, args, /*is_dlx=*/false);
    }

    /* brief: 普通发布（无确认） */
    bool publish(const std::string &exchange, const std::string &routing_key, const std::string &body) {
        post_task([this, exchange, routing_key, body]() {
            _channel.publish(exchange, routing_key, body);
        });
        return true;
    }

    /* brief: 带 broker 确认的发布；callback 在 ack/nack/lost/error 时被调用 */
    void publish_confirm(const std::string &exchange,
                         const std::string &routing_key,
                         const std::string &body,
                         const PublishConfirmCallback &callback)
    {
        if(!_reliable) {
            if(callback) callback(PublishStatus::Error, "发布确认未启用");
            return;
        }
        post_task([this, exchange, routing_key, body, callback]() {
            _reliable->publish(exchange, routing_key, body)
                .onAck([callback]() {
                    if(callback) callback(PublishStatus::Acked, "broker 已确认");
                })
                .onNack([callback]() {
                    if(callback) callback(PublishStatus::Nacked, "broker 显式拒绝");
                })
                .onLost([callback]() {
                    if(callback) callback(PublishStatus::Lost, "通道断开，状态未知");
                })
                .onError([callback](const char *msg) {
                    if(callback) callback(PublishStatus::Error, msg ? msg : "未知错误");
                });
        });
    }

    /* brief: 订阅队列；如果是 delayed 模式应订阅 DLX 队列（外层 Subscriber 已处理） */
    bool consume(const std::string &queue, const MessageCallback &callback,
                 uint16_t prefetch = kDefaultPrefetch)
    {
        std::promise<bool> promise;
        auto future = promise.get_future();

        post_task([this, queue, callback, prefetch, &promise]() {
            // 流控：限制未 ack 的最大消息数，防慢消费打爆内存
            _channel.setQos(prefetch);
            _channel.consume(queue)
                .onMessage([this, callback](const AMQP::Message &message,
                                            uint64_t deliveryTag,
                                            bool redelivered) {
                    try {
                        ConsumeAction action = callback(message.body(), message.bodySize(), redelivered);
                        switch(action) {
                        case ConsumeAction::Ack:         _channel.ack(deliveryTag); break;
                        case ConsumeAction::NackRequeue: _channel.reject(deliveryTag, true);  break;
                        case ConsumeAction::NackDiscard: _channel.reject(deliveryTag, false); break;
                        }
                    } catch(const std::exception &e) {
                        LOG_ERROR("消费回调异常: {}", e.what());
                        _channel.reject(deliveryTag, true);
                    } catch(...) {
                        LOG_ERROR("消费回调发生未知异常");
                        _channel.reject(deliveryTag, true);
                    }
                })
                .onError([&promise, queue](const char *message) {
                    LOG_ERROR("订阅消息失败: {} - {}", queue, message ? message : "");
                    promise.set_value(false);
                })
                .onSuccess([&promise, queue]() {
                    LOG_DEBUG("成功订阅队列: {}", queue);
                    promise.set_value(true);
                });
        });
        return future.get();
    }

    void wait() { _async_thread.join(); }

private:
    static void quit_callback(struct ev_loop *loop, ev_async *, int) {
        ev_break(loop, EVBREAK_ALL);
    }

    static void task_callback(struct ev_loop *, ev_async *watcher, int) {
        auto *self = static_cast<MQClient*>(watcher->data);
        std::queue<std::function<void()>> tasks;
        {
            std::lock_guard<std::mutex> lk(self->_task_mtx);
            tasks.swap(self->_task_queue);
        }
        while(!tasks.empty()) {
            tasks.front()();
            tasks.pop();
        }
    }

    void post_task(std::function<void()> task) {
        {
            std::lock_guard<std::mutex> lk(_task_mtx);
            _task_queue.push(std::move(task));
        }
        ev_async_send(_ev_loop, &_task_watcher);
    }

    /* brief: 声明 exchange + queue + bind 链路；返回是否成功 */
    bool _declared(const declare_settings &settings, AMQP::Table args, bool is_dlx) {
        std::promise<bool> promise;
        auto future = promise.get_future();

        post_task([this, settings, args, is_dlx, &promise]() mutable {
            std::string exchange    = is_dlx ? settings.dlx_exchange()    : settings.exchange;
            std::string queue       = is_dlx ? settings.dlx_queue()       : settings.queue;
            std::string binding_key = is_dlx ? settings.dlx_binding_key() : settings.binding_key;
            // delayed 模式底层走 direct；DLX 链路也用 direct
            AMQP::ExchangeType type = is_dlx ? AMQP::ExchangeType::direct
                                             : exchange_type(settings.exchange_type);

            _channel.declareExchange(exchange, type)
                .onSuccess([this, queue, args, exchange, binding_key, &promise]() mutable {
                    _channel.declareQueue(queue, args)
                        .onSuccess([this, exchange, queue, binding_key, &promise](
                                       const std::string &, uint32_t, uint32_t) {
                            _channel.bindQueue(exchange, queue, binding_key)
                                .onSuccess([&promise](){ promise.set_value(true); })
                                .onError([&promise, exchange, queue](const char *m){
                                    LOG_ERROR("绑定 {}-{} 失败: {}", exchange, queue, m ? m : "");
                                    promise.set_value(false);
                                });
                        })
                        .onError([&promise, queue](const char *m){
                            LOG_ERROR("声明队列 {} 失败: {}", queue, m ? m : "");
                            promise.set_value(false);
                        });
                })
                .onError([&promise, exchange](const char *m){
                    LOG_ERROR("声明交换机 {} 失败: {}", exchange, m ? m : "");
                    promise.set_value(false);
                });
        });
        return future.get();
    }

    std::mutex _task_mtx;
    std::queue<std::function<void()>> _task_queue;

    struct ev_loop *_ev_loop;
    struct ev_async _ev_async;
    struct ev_async _task_watcher;

    AMQP::LibEvHandler _handler;
    AMQP::TcpConnection _connection;
    AMQP::TcpChannel _channel;
    std::unique_ptr<AMQP::Reliable<>> _reliable;
    std::thread _async_thread;
};

/* brief: 单一交换机/队列的发布者 */
class Publisher
{
public:
    using ptr = std::shared_ptr<Publisher>;
    Publisher(const MQClient::ptr &mq, const declare_settings &settings)
        : _mq(mq), _settings(settings) { _mq->declare(settings); }

    bool publish(const std::string &body) {
        return _mq->publish(_settings.exchange, _settings.binding_key, body);
    }
    void publish_confirm(const std::string &body, const PublishConfirmCallback &cb) {
        _mq->publish_confirm(_settings.exchange, _settings.binding_key, body, cb);
    }
private:
    MQClient::ptr _mq;
    declare_settings _settings;
};

/* brief: 单一交换机/队列的订阅者；delayed 模式自动订阅 DLX 队列 */
class Subscriber
{
public:
    using ptr = std::shared_ptr<Subscriber>;
    Subscriber(const MQClient::ptr &mq, const declare_settings &settings, const MessageCallback &cb)
        : _mq(mq), _settings(settings), _callback(cb) { _mq->declare(settings); }

    void consume(MessageCallback &&cb, uint16_t prefetch = kDefaultPrefetch) {
        _callback = std::move(cb);
        const std::string &q = (_settings.exchange_type == DELAYED)
            ? _settings.dlx_queue() : _settings.queue;
        _mq->consume(q, _callback, prefetch);
    }
private:
    MQClient::ptr _mq;
    declare_settings _settings;
    MessageCallback _callback;
};

class MQFactory
{
public:
    template <typename T, typename... Args>
    static std::shared_ptr<T> create(Args&&... args) {
        return std::make_shared<T>(std::forward<Args>(args)...);
    }
};

} // namespace chatnow
