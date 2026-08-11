#include "direct_transport.hpp"

#include "endpoint.hpp"
#include "policy.hpp"
#include "../layouts/gog.hpp"

#include <utility>

namespace fusioncutter::patches::direct_transport::server {

DirectTransportServer::DirectTransportServer(DirectTransportSettings settings, const TargetContext& target)
    : layout_(layouts::kGogGame), policy_(settings.policy), transport_(target.image, layout_),
      hooks_(target.image, layout_) {
    auto resolved = resolve_policy(settings.policy);
    if (!resolved.has_value()) {
        policy_error_ = std::move(resolved.error());
    } else {
        policy_ = *resolved;
    }
    request_endpoint_observer(!policy_error_.has_value() && policy_ != Policy::Disabled);
}

DirectTransportServer::~DirectTransportServer() {
    disable_runtime();
}

void DirectTransportServer::build_plan(PatchPlan& plan) {
    if (policy_error_.has_value() || policy_ == Policy::Disabled) {
        return;
    }
    hooks_.build_plan(plan);
    transport_.build_plan(plan);
}

std::expected<void, OutcomeReason> DirectTransportServer::prepare_runtime() {
    if (policy_error_.has_value()) {
        return std::unexpected(std::move(*policy_error_));
    }
    return transport_.prepare(policy_);
}

void DirectTransportServer::enable_runtime() noexcept {
    if (policy_ == Policy::Disabled) {
        return;
    }
    publish_endpoint_sink(transport_);
    hooks_.enable(transport_);
    enabled_ = true;
}

void DirectTransportServer::disable_runtime() noexcept {
    if (!enabled_) {
        return;
    }
    hooks_.disable(transport_);
    clear_endpoint_sink(transport_);
    transport_.shutdown();
    enabled_ = false;
}

void DirectTransportServer::write_status(StatusSection& output) const noexcept {
    transport_.write_status(output);
}

} // namespace fusioncutter::patches::direct_transport::server
