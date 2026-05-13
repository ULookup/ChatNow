#pragma once

#include <brpc/controller.h>
#include <google/protobuf/service.h>
#include <functional>

namespace chatnow
{

/* brief: 自删 brpc 异步 Closure
 *  - brpc::NewCallback 仅接受 free-function-pointer，无法绑 capturing lambda；
 *    本 Closure 把 cntl/req/rsp 与回调一并放进 this，Run() 后 delete this。
 *  - 使用方式：
 *      auto *c = new SelfDeleteRpcClosure<Req, Rsp>();
 *      c->req.set_xxx(...);
 *      c->on_done = [...](brpc::Controller *cntl, const Rsp &rsp) { ... };
 *      stub.SomeRpc(&c->cntl, &c->req, &c->rsp, c);
 *  - on_done 可空（true fire-and-forget）；调用方需在回调内自行检查 cntl->Failed()
 */
template <typename Req, typename Rsp>
class SelfDeleteRpcClosure final : public ::google::protobuf::Closure
{
public:
    brpc::Controller cntl;
    Req req;
    Rsp rsp;
    std::function<void(brpc::Controller*, const Rsp&)> on_done;

    void Run() override {
        if(on_done) on_done(&cntl, rsp);
        delete this;
    }
};

} // namespace chatnow
