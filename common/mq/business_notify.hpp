#pragma once

#include <string>
#include <vector>

#include "auth/forward_auth.hpp"
#include "infra/logger.hpp"
#include "mq/channel.hpp"
#include "push/push_service.pb.h"

namespace chatnow {

inline constexpr const char* kBusinessPushService = "/service/push_service";

// Domain writes are already committed. Online notifications are best effort;
// clients retain the domain APIs as their durable source of truth.
inline void notify_online_users(const ServiceManager::ptr& channels,
                                brpc::Controller* incoming,
                                const std::string& request_id,
                                const std::vector<std::string>& recipients,
                                const ::chatnow::push::NotifyMessage& notification) {
    if (recipients.empty()) return;
    auto channel = channels->choose(kBusinessPushService);
    if (!channel) {
        LOG_WARN("Business notification unavailable: no Push channel");
        return;
    }
    ::chatnow::push::PushService_Stub stub(channel.get());
    ::chatnow::push::PushBatchReq request;
    ::chatnow::push::PushBatchRsp response;
    request.set_request_id(request_id);
    for (const auto& recipient : recipients) request.add_user_id_list(recipient);
    request.mutable_notify()->CopyFrom(notification);
    brpc::Controller outgoing;
    outgoing.set_timeout_ms(1000);
    ::chatnow::auth::forward_auth_metadata(incoming, &outgoing);
    stub.PushBatch(&outgoing, &request, &response, nullptr);
    if (outgoing.Failed() || !response.header().success()) {
        LOG_WARN("Business notification delivery failed");
    }
}

}  // namespace chatnow
