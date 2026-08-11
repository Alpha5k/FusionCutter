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
    : layout_(layout_for(target.layout)), transport_(target.image, layout_), hooks_(target.image, layout_) {}

DirectTransportClient::~DirectTransportClient() {
    disable_runtime();
}

void DirectTransportClient::build_plan(PatchPlan& plan) {
    hooks_.build_plan(plan);
}

std::expected<void, OutcomeReason> DirectTransportClient::prepare_runtime() {
    return transport_.prepare();
}

void DirectTransportClient::enable_runtime() noexcept {
    hooks_.enable(transport_);
}

void DirectTransportClient::disable_runtime() noexcept {
    hooks_.disable(transport_);
    transport_.shutdown();
}

void DirectTransportClient::write_status(StatusSection& output) const noexcept {
    transport_.write_status(output);
}

} // namespace fusioncutter::patches::direct_transport::client
