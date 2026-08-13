#include "direct_transport.hpp"

#include "../layouts/gog.hpp"
#include "../layouts/steam.hpp"

#include <utility>

namespace fusioncutter::patches::direct_transport::client {
namespace {

[[nodiscard]] const GameLayout& layout_for(TargetLayout layout) noexcept {
    switch (layout) {
    case TargetLayout::GOGRetail:
        return layouts::kGogGame;
    case TargetLayout::SteamRetail:
        return layouts::kSteamGame;
    case TargetLayout::Aspyr:
    case TargetLayout::ModTools:
        std::unreachable();
    }
    std::unreachable();
}

} // namespace

DirectTransportClient::DirectTransportClient(const TargetContext& target) noexcept
    : layout_(layout_for(target.layout)), transport_(target.image, layout_), lobby_hooks_(target.image, layout_),
      pipeline_callbacks_(make_pipeline_callbacks(transport_)) {}

DirectTransportClient::~DirectTransportClient() {
    disable_runtime();
}

void DirectTransportClient::build_plan(PatchPlan& plan) {
    lobby_hooks_.build_plan(plan);
}

std::expected<void, OutcomeReason> DirectTransportClient::prepare_runtime() {
    return transport_.prepare();
}

void DirectTransportClient::enable_runtime() noexcept {
    lobby_hooks_.enable(transport_);
    network_pipeline::publish_transport(pipeline_callbacks_);
}

void DirectTransportClient::disable_runtime() noexcept {
    network_pipeline::clear_transport(pipeline_callbacks_);
    lobby_hooks_.disable(transport_);
    transport_.shutdown();
}

void DirectTransportClient::write_status(StatusSection& output) const noexcept {
    transport_.write_status(output);
}

} // namespace fusioncutter::patches::direct_transport::client
