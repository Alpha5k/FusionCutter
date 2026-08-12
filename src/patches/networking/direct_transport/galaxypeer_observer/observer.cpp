#include "observer.hpp"

#include "../server/endpoint.hpp"
#include "../shared/socket.hpp"

#include <WinSock2.h>
#include <Windows.h>

#include <cstddef>
#include <utility>

namespace fusioncutter::patches::galaxypeer_observer {
namespace {

// Models the IPv4 fields returned by Galaxy's external-address query.
struct GalaxySystemAddress {
    std::uint16_t family;
    std::uint16_t port_network_order;
    std::uint32_t address_network_order;
    std::byte reserved[12];
};

static_assert(sizeof(GalaxySystemAddress) == 20);

using GetExternalId = GalaxySystemAddress*(__thiscall*)(void*, GalaxySystemAddress*, GalaxySystemAddress);

constexpr GalaxyPeerObserver::Layout kGalaxy2017{0x0048'A970, 0x0087'AE64};
constexpr GalaxyPeerObserver::Layout kGalaxy2018{0x0047'5C50, 0x008B'107C};
constexpr auto kGetExternalIdPreimage = byte_array<0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x14, 0x57, 0x8B>();

PatchInstanceSlot<GalaxyPeerObserver> gActive;
OriginalFunction<GetExternalId> gOriginal;

// Forwards Galaxy's external-address result without changing caller-visible error state.
GalaxySystemAddress* __fastcall hook_get_external_id(void* raw_peer, void*, GalaxySystemAddress* output,
                                                     GalaxySystemAddress observer) noexcept {
    const auto original = gOriginal.get();
    auto* result = original == nullptr ? nullptr : original(raw_peer, output, observer);
    const auto windows_error = GetLastError();
    const auto socket_error = WSAGetLastError();
    if (auto* active = gActive.read(); active != nullptr) {
        active->observe(raw_peer, result);
    }
    WSASetLastError(socket_error);
    SetLastError(windows_error);
    return result;
}

[[nodiscard]] GalaxyPeerObserver::Layout layout_for(std::size_t image_size) noexcept {
    switch (image_size) {
    case 0x00AC'7000:
        return kGalaxy2017;
    case 0x00AF'3000:
        return kGalaxy2018;
    default:
        std::unreachable();
    }
}

} // namespace

GalaxyPeerObserver::GalaxyPeerObserver(const TargetContext& target) noexcept
    : image_(target.image), layout_(layout_for(target.image.size)),
      requested_(direct_transport::server::endpoint_observer_requested()) {}

GalaxyPeerObserver::~GalaxyPeerObserver() {
    disable_runtime();
}

void GalaxyPeerObserver::build_plan(PatchPlan& plan) {
    if (!requested_) {
        return;
    }
    gOriginal = plan.inline_hook_with_original("Observe the server public endpoint", layout_.get_external_id_rva,
                                               BytePattern::exact(kGetExternalIdPreimage),
                                               reinterpret_cast<GetExternalId>(&hook_get_external_id));
}

void GalaxyPeerObserver::enable_runtime() noexcept {
    if (requested_) {
        gActive.publish(*this);
    }
}

void GalaxyPeerObserver::disable_runtime() noexcept {
    gActive.clear(*this);
}

void GalaxyPeerObserver::observe(void* raw_peer, const void* raw_result) noexcept {
    const auto* result = static_cast<const GalaxySystemAddress*>(raw_result);
    if (raw_peer == nullptr || result == nullptr) {
        return;
    }
    const auto expected_vtable = image_.address_at_rva(layout_.raw_vtable_rva, sizeof(std::uintptr_t));
    if (*static_cast<const std::uintptr_t*>(raw_peer) != expected_vtable) {
        return;
    }
    if (result->family == AF_INET) {
        if (direct_transport::is_public_ipv4(result->address_network_order)) {
            direct_transport::server::publish_public_endpoint(result->address_network_order);
        } else if (result->address_network_order != 0 && result->address_network_order != INADDR_NONE) {
            direct_transport::server::publish_unusable_endpoint();
        }
    }
}

} // namespace fusioncutter::patches::galaxypeer_observer
