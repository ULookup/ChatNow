#pragma once
#include <ev.h>
#include <amqpcpp.h>
#include <amqpcpp/libev.h>
#include <openssl/ssl.h>
#include <openssl/opensslv.h>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <queue>
#include <functional>
#include <future>
#include "logger.hpp"

namespace chatnow
{

const std::string DIRECT = "direct";
const std::string FANOUT = "fanout";
const std::string TOPIC = "topic";
const std::string HEADERS = "headers";
const std::string DELAYED = "delayed";
const std::string DLX_PREFIX = "dlx_";
const std::string DEAD_LETTER_EXCHANGE = "x-dead-letter-exchange";
const std::string DEAD_LETTER_ROUTING_KEY = "x-dead-letter-routing-key";
const std::string MESSAGE_TTL = "x-message-ttl";

struct declare_settings {
    std::string exchange;
    // 交换机类型: direct, fanout, topic, delayed
    std::string exchange_type;
    std::string queue;
    std::string binding_key;
    size_t delayed_ttl = 5; // 延时消息的 TTL，单位秒，默认5秒

    std::string dlx_exchange() const { return exchange + DLX_PREFIX; }
    std::string dlx_queue() const { return queue + DLX_PREFIX; }
    std::string dlx_binding_key() const { return binding_key + DLX_PREFIX; }
};

AMQP::ExchangeType exchange_type(const std::string &type) {
    if (type == DIRECT) {
        return AMQP::ExchangeType::direct;
    } else if (type == FANOUT) {
        return AMQP::ExchangeType::fanout;
    } else if (type == TOPIC) {
        return AMQP::ExchangeType::topic;
    } else if (type == HEADERS) {
        return AMQP::ExchangeType::headers;
    } else if (type == DELAYED) {
        return AMQP::ExchangeType::direct;
    }
    LOG_ERROR("无效的交换机类型: {}", type);
    abort();
}

enum class PublishStatus {
    Acked,   // broker 确认接收
    Nacked,  // broker 显式拒绝
    Lost,    // nack 或连接/通道问题导致丢失
    Error    // publish 过程中发生 channel error
};

enum class ConsumeAction {
    Ack,            // 处理成功，确认消费
    NackRequeue,    // 处理失败，重新入队
    NackDiscard     // 处理失败，丢弃/交给死信
};

using MessageCallback = std::function<ConsumeAction(const char*, size_t, bool redelivered)>;
using PublishConfirmCallback = std::function<void(PublishStatus, const std::string&)>;

class MQClient
{
public:
    using ptr = std::shared_ptr<MQClient>;

    MQClient(const std::string &url)
        // 建议使用 ev_loop_new 避免与系统其他使用 EV_DEFAULT 的库发生冲突
        : _ev_loop(ev_loop_new(EVFLAG_AUTO))
        , _handler(_ev_loop)
        , _connection(&_handler, AMQP::Address(url))
        , _channel(&_connection)
        , _reliable(std::make_unique<AMQP::Reliable<>>(_channel))
    {
        // 1. 初始化退出事件观察者
        ev_async_init(&_ev_async, quit_callback);
        ev_async_start(_ev_loop, &_ev_async);

        // 2. 初始化任务队列事件观察者（跨线程唤醒核心）
        _task_watcher.data = this;
        ev_async_init(&_task_watcher, task_callback);
        ev_async_start(_ev_loop, &_task_watcher);

        // 3. 一切准备就绪后，最后启动 libev 事件循环线程
        _async_thread = std::thread([this](){ ev_run(_ev_loop, 0); });
    }

    ~MQClient() {
        // 发送退出信号唤醒事件循环
        ev_async_send(_ev_loop, &_ev_async);
        if (_async_thread.joinable()) {
            _async_thread.join();
        }
        // 清理 loop
        ev_loop_destroy(_ev_loop);
    }

    void declare(const declare_settings &settings) {
        AMQP::Table args;
        if(settings.exchange_type == DELAYED) {
            _declared(settings, args, true);
            args[MESSAGE_TTL] = settings.delayed_ttl;
            args[DEAD_LETTER_EXCHANGE] = settings.dlx_exchange();
            args[DEAD_LETTER_ROUTING_KEY] = settings.dlx_binding_key();
        }
        _declared(settings, args, false);
    }

    bool publish(const std::string &exchange, const std::string &routing_key, const std::string &body) {
        // 将发布操作派发到 libev 线程执行
        post_task([this, exchange, routing_key, body]() {
            _channel.publish(exchange, routing_key, body);
        });
        return true; 
    }

    void publish_confirm(const std::string &exchange,
                         const std::string &routing_key,
                         const std::string &body,
                         const PublishConfirmCallback &callback)
    {
        if (!_reliable) {
            if (callback) callback(PublishStatus::Error, "发布确认未启用");
            return;
        }

        // 将发布确认操作打包放入任务队列，唤醒 libev 线程执行
        post_task([this, exchange, routing_key, body, callback]() {
            _reliable->publish(exchange, routing_key, body)
                .onAck([callback]() {
                    if (callback) callback(PublishStatus::Acked, "消息被 broker 确认");
                })
                .onNack([callback]() {
                    if (callback) callback(PublishStatus::Nacked, "消息被 broker 拒绝");
                })
                .onLost([callback]() {
                    if (callback) callback(PublishStatus::Lost, "消息丢失");
                })
                .onError([callback](const char *message) {
                    if (callback) callback(PublishStatus::Error, message ? message : "未知错误");
                });
        });
    }

    void consume(const std::string &queue, const MessageCallback &callback) {
        std::promise<void> promise;
        auto future = promise.get_future();

        post_task([this, queue, callback, &promise]() {
            _channel.consume(queue)
                .onMessage([this, callback](const AMQP::Message &message,
                                            uint64_t deliveryTag,
                                            bool redelivered) {
                    try {
                        ConsumeAction action = callback(message.body(), message.bodySize(), redelivered);
                        switch (action) {
                        case ConsumeAction::Ack:
                            _channel.ack(deliveryTag);
                            break;
                        case ConsumeAction::NackRequeue:
                            _channel.reject(deliveryTag, true);
                            break;
                        case ConsumeAction::NackDiscard:
                            _channel.reject(deliveryTag, false);
                            break;
                        default:
                            LOG_ERROR("未知的 ConsumeAction");
                            _channel.reject(deliveryTag, true);
                            break;
                        }
                    }
                    catch (const std::exception &e) {
                        LOG_ERROR("消费回调异常: {}", e.what());
                        _channel.reject(deliveryTag, true);
                    }
                    catch (...) {
                        LOG_ERROR("消费回调发生未知异常");
                        _channel.reject(deliveryTag, true);
                    }
                })
                .onError([&promise, queue](const char *message) {
                    LOG_ERROR("订阅消息失败: {} - {}", queue, message);
                    promise.set_value(); // 解除阻塞
                    abort();
                })
                .onSuccess([&promise, queue]() {
                    LOG_DEBUG("成功订阅队列: {}", queue);
                    promise.set_value(); // 成功，解除阻塞
                });
        });

        future.get(); // 阻塞等待 libev 线程完成订阅操作
    }

    void wait() { _async_thread.join(); }

private:
    // 退出事件回调
    static void quit_callback(struct ev_loop *loop, ev_async *watcher, int32_t revents) {
        ev_break(loop, EVBREAK_ALL);
    }

    // 处理任务队列的回调（运行在 libev 线程中）
    static void task_callback(struct ev_loop *loop, ev_async *watcher, int32_t revents) {
        MQClient *self = static_cast<MQClient*>(watcher->data);
        std::queue<std::function<void()>> tasks;
        {
            // 快速加锁，取出所有任务
            std::lock_guard<std::mutex> lock(self->_task_mtx);
            tasks.swap(self->_task_queue);
        }
        // 在无锁状态下，单线程依次执行所有 AMQP 操作
        while (!tasks.empty()) {
            tasks.front()();
            tasks.pop();
        }
    }

    // 统一的任务派发接口
    void post_task(std::function<void()> task) {
        {
            std::lock_guard<std::mutex> lock(_task_mtx);
            _task_queue.push(std::move(task));
        }
        // 唤醒 libev 所在的底层 epoll_wait
        ev_async_send(_ev_loop, &_task_watcher);
    }

    void _declared(const declare_settings &settings, AMQP::Table args, bool is_dlx = false) {
        std::mutex mtx;           // 防止并发 Declare 的粗粒度锁
        std::lock_guard<std::mutex> declare_lock(mtx); 

        std::promise<void> promise;
        auto future = promise.get_future();

        post_task([this, settings, args, is_dlx, &promise]() mutable {
            std::string exchange = is_dlx ? settings.dlx_exchange() : settings.exchange;
            std::string queue = is_dlx ? settings.dlx_queue() : settings.queue;
            std::string binding_key = is_dlx ? settings.dlx_binding_key() : settings.binding_key;
            AMQP::ExchangeType type = exchange_type(settings.exchange_type);

            _channel.declareExchange(exchange, type)
                .onSuccess([this, queue, args, exchange, binding_key, &promise]() mutable {
                    _channel.declareQueue(queue, args)
                        .onSuccess([this, exchange, queue, binding_key, &promise](const std::string &, uint32_t, uint32_t) {
                            _channel.bindQueue(exchange, queue, binding_key)
                                .onSuccess([&promise](){
                                    promise.set_value(); // 链路全部成功，解除阻塞
                                })
                                .onError([&promise, exchange, queue](const char *message){
                                    LOG_ERROR("绑定交换机和队列失败:{} - {} - {}", exchange, queue, message);
                                    promise.set_value(); 
                                    abort();
                                });
                        })
                        .onError([&promise, queue](const char *message){
                            LOG_ERROR("声明队列失败: {} - {}", queue, message);
                            promise.set_value();
                            abort();
                        });
                })
                .onError([&promise, exchange](const char *message){
                    LOG_ERROR("声明交换机失败: {} - {}", exchange, message);
                    promise.set_value();
                    abort();
                });
        });

        // 阻塞当前线程，直到异步的 declare 回调链执行完毕并 set_value
        future.get();
    }

private:
    std::mutex _task_mtx;                        // 任务队列互斥锁
    std::queue<std::function<void()>> _task_queue; // 任务队列，存放要在 libev 线程执行的操作

    struct ev_loop *_ev_loop;    
    struct ev_async _ev_async;   // 用于退出的 watcher
    struct ev_async _task_watcher; // 用于唤醒并执行任务队列的 watcher

    AMQP::LibEvHandler _handler; 
    AMQP::TcpConnection _connection; 
    AMQP::TcpChannel _channel;       
    std::unique_ptr<AMQP::Reliable<>> _reliable; 
    std::thread _async_thread; 
};

class Publisher
{
public:
    using ptr = std::shared_ptr<Publisher>;
    /* brief: 构造函数，初始化MQ客户端，并声明交换机和队列 */
    Publisher(const MQClient::ptr &mq_client, const declare_settings &settings) : _mq_client(mq_client), _settings(settings) {
        _mq_client->declare(settings);
    }
    bool publish(const std::string &body) {
        return _mq_client->publish(_settings.exchange, _settings.binding_key, body);
    }
    void publish_confirm(const std::string &body, const PublishConfirmCallback &callback) {
        _mq_client->publish_confirm(_settings.exchange, _settings.binding_key, body, callback);
    }
private:
    MQClient::ptr _mq_client;
    declare_settings _settings;
};

class Subscriber
{
public:
    using ptr = std::shared_ptr<Subscriber>;
    /* brief: 构造函数，初始化MQ客户端，并声明交换机和队列 */
    Subscriber(const MQClient::ptr &mq_client, const declare_settings &settings, const MessageCallback &callback) : _mq_client(mq_client), _settings(settings), _callback(callback) {
        _mq_client->declare(settings);
    }
    /* brief: 订阅消息时，如果是延时队列，则实际订阅的是配套的死信队列 */
    void consume(MessageCallback &&callback) {
        _callback = std::move(callback);
        // 如果是延时队列，则订阅死信队列
        if(_settings.exchange_type == "delayed") {
            _mq_client->consume(_settings.dlx_queue(), _callback);
        } else {
            _mq_client->consume(_settings.queue, _callback);
        }
    }
private:
    MQClient::ptr _mq_client;
    declare_settings _settings;
    MessageCallback _callback;
};

class MQFactory
{
public:
    // 提供一个通用的工厂，实现各个不同的类对象的创建
    template<typename T, typename... Args>
    static std::shared_ptr<T> create(Args&&... args) {
        return std::make_shared<T>(std::forward<Args>(args)...);
    }
};

} // namespace chatnow