#include <ev.h>
#include <amqpcpp.h>
#include <amqpcpp/libev.h>
#include <openssl/ssl.h>
#include <openssl/opensslv.h>
#include "logger.hpp"

namespace chatnow
{

class MQClient
{
public:
    using MessageCallback = std::function<void(const char*, size_t)>;

    MQClient(const std::string &user, const std::string &passwd, const std::string &host) {
        _loop = EV_DEFAULT;
        _handler = std::make_unique<AMQP::LibEvHandler>(_loop);
        std::string url = "amqp://" + user + ":" + passwd + "@" + host + "/";
        AMQP::Address address(url);
        _connection = std::make_unique<AMQP::TcpConnection>(_handler.get(), address);
        _channel = std::make_unique<AMQP::TcpChannel>(_connection.get());

        _loop_thread = std::thread([this](){
            ev_run(_loop);
        });
    }
    ~MQClient() {
        ev_async_init(&_async_watcher, watcher_callback);
        ev_async_start(_loop, &_async_watcher);
        ev_async_send(_loop, &_async_watcher);
        _loop_thread.join();
        _loop = nullptr;
    }
    void declareComponents(const std::string &exchange,
                        const std::string &queue, 
                        const std::string &routing_key = "routing_key", 
                        AMQP::ExchangeType exchange_type = AMQP::ExchangeType::direct)
    {
        _channel->declareExchange(exchange, exchange_type)
                .onError([](const char *message){
                    LOG_ERROR("声明交换机失败: {}", message);
                    exit(0);
                })
                .onSuccess([exchange](){
                    LOG_DEBUG("{} 交换机创建成功!", exchange);
                });
        _channel->declareQueue(queue)
                .onError([](const char *message){
                    LOG_ERROR("声明队列失败: {}", message);
                })
                .onSuccess([queue](){
                    LOG_DEBUG("{} 队列创建成功!", queue);
                });
        // step2: 针对交换机和队列进行绑定
        _channel->bindQueue(exchange, queue, routing_key)
                .onError([exchange, queue](const char *message){
                    LOG_ERROR("{} - {} 绑定失败: {}", exchange, queue, message);
                })
                .onSuccess([exchange, queue](){
                    LOG_DEBUG("{} - {} 绑定成功!", exchange, queue);
                });
    }
    bool publish(const std::string &exchange, 
                const std::string &msg, 
                const std::string &routing_key = "routing_key") 
    {
        bool ret = _channel->publish(exchange, routing_key, msg);
        if(ret == false) {
            LOG_ERROR("{} 发布消息失败: ", exchange);
            return false;
        }
        return true;
    }
    void consume(const std::string &queue, const MessageCallback &cb) {
        auto callback = std::bind(cb, &_channel, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3);
        _channel->consume(queue, "consume-test")
                .onReceived([this, cb](const AMQP::Message &message, uint64_t deliveryTag, bool redelivered){
                    cb(message.body(), message.bodySize());
                    _channel->ack(deliveryTag);
                })
                .onError([queue](const char *message){
                    LOG_ERROR("订阅 {} 队列消息失败: {}", queue, message);
                });
    }
private:
    static void watcher_callback(struct ev_loop *loop, ev_async *watcher, int32_t revents) {
        ev_break(loop, EVBREAK_ALL);
    }
private:
    struct ev_async _async_watcher;
    struct ev_loop *_loop;
    std::unique_ptr<AMQP::LibEvHandler> _handler;
    std::unique_ptr<AMQP::TcpConnection> _connection;
    std::unique_ptr<AMQP::TcpChannel> _channel;
    std::thread _loop_thread;
};

}