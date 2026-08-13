#include "transport.hpp"

#include "../shared/datagram.hpp"

#include <WinSock2.h>
#include <Windows.h>

#include <cstring>

namespace fusioncutter::patches::direct_transport::client {
namespace {

constexpr std::uint32_t kMemberDeparture = 0x02 | 0x04 | 0x08 | 0x10;

} // namespace

void ClientTransport::before_receive() noexcept {
    if (!claim_network_thread("Pump client network receive")) {
        return;
    }
    const auto now = GetTickCount64();
    pump_control(now);
    pump_direct(now);
}

void ClientTransport::after_receive() noexcept {
    if (on_network_thread("Service client association")) {
        service_association(GetTickCount64());
    }
}

void ClientTransport::on_native_transmit(int physical_primary) noexcept {
    if (physical_primary != kHostPrimary || !on_network_thread("Observe client native transmit")) {
        return;
    }
    std::uint64_t lobby_id{};
    if (!read_identity(kHostPrimary, lobby_id) || (association_.live && association_.lobby_id == lobby_id)) {
        return;
    }
    std::uint64_t owner_user_id{};
    if (!read_lobby_owner(lobby_id, owner_user_id)) {
        return;
    }
    invalidate_association(AssociationEndReason::Replaced);
    start_association(lobby_id, owner_user_id, GetTickCount64());
}

int ClientTransport::begin_transmit_group(int physical_primary, int) noexcept {
    if (physical_primary != kHostPrimary || !on_network_thread("Begin client transmit group") || !association_.live ||
        !current_host(association_.lobby_id)) {
        return -1;
    }
    if (!association_.transmit_group.active() && association_.transmit_route == TransmitRoute::DirectPending) {
        set_route(association_.state, TransmitRoute::Direct, association_.receive_permission);
    }
    const auto carrier = association_.transmit_route == TransmitRoute::Direct ? Carrier::Direct : Carrier::Galaxy;
    if (!association_.transmit_group.begin(association_.generation, carrier)) {
        runtime_log_.error("The client transmit-group depth overflowed", "Begin client transmit group");
        return -1;
    }
    return kHostPrimary;
}

void ClientTransport::end_transmit_group(int physical_primary) noexcept {
    if (physical_primary == kHostPrimary && on_network_thread("End client transmit group") &&
        !association_.transmit_group.end()) {
        runtime_log_.error("A client transmit group ended without a matching begin", "End client transmit group");
    }
}

NativeTransmitResult ClientTransport::transmit_native(int physical_primary, int group_primary,
                                                      std::span<const std::uint8_t> bytes) noexcept {
    if (group_primary >= 0) {
        physical_primary = group_primary;
    }
    if (physical_primary != kHostPrimary) {
        return {};
    }
    if (!on_network_thread("Transmit client native packet")) {
        return {.result = -1, .error = WSAENOTCONN, .handled = true};
    }

    const auto grouped = association_.transmit_group.active();
    const auto stale_group = grouped && !association_.transmit_group.belongs_to(association_.generation);
    if (stale_group) {
        runtime_log_.error("A client transmit group belongs to a stale association generation",
                           "Transmit client native packet");
    }
    if (!association_.live || !current_host(association_.lobby_id) || stale_group) {
        return grouped || association_.transmit_route != TransmitRoute::Galaxy
                   ? NativeTransmitResult{.result = -1, .error = WSAENOTCONN, .handled = true}
                   : NativeTransmitResult{};
    }

    auto carrier = grouped ? association_.transmit_group.carrier() : Carrier::Galaxy;
    if (!grouped) {
        if (association_.transmit_route == TransmitRoute::DirectPending) {
            set_route(association_.state, TransmitRoute::Direct, association_.receive_permission);
        }
        if (association_.transmit_route == TransmitRoute::Direct) {
            carrier = Carrier::Direct;
        }
    }
    if (carrier != Carrier::Direct) {
        return {};
    }
    if (association_.state != RouteState::DirectLocked || !association_.server_endpoint.valid() ||
        !socket_.available()) {
        runtime_log_.error("The client selected Direct without a usable transport", "Validate client direct route");
        return {.result = -1, .error = WSAENOTCONN, .handled = true};
    }
    const auto result =
        send_direct_data(socket_, send_buffer_, Direction::ClientToServer, association_.session_key,
                         association_.connection_id, association_.send_sequence++, association_.server_endpoint, bytes);
    if (result.result < 0) {
        association_.diagnostics.send_failed();
        if (result.failure == NativeTransmitResult::Failure::Serialization) {
            runtime_log_.error("The native game packet could not be serialized within protocol bounds",
                               "Serialize direct game data");
        } else if (result.failure == NativeTransmitResult::Failure::PartialSend) {
            runtime_log_.error("The UDP socket reported a partial datagram send", "Complete direct UDP send");
        } else {
            runtime_log_.socket("Send direct UDP data", result.error, is_internal_socket_error(result.error));
        }
    } else {
        association_.diagnostics.direct_tx("Client", kHostPrimary, association_.generation, association_.connection_id);
    }
    return result;
}

void ClientTransport::on_native_intake(void* endpoint) noexcept {
    if (endpoint == nullptr || !on_network_thread("Observe client native intake")) {
        return;
    }
    std::uint64_t sender{};
    std::memcpy(&sender, endpoint, sizeof(sender));
    std::uint64_t lobby_id{};
    if (sender == 0 || !read_identity(kHostPrimary, lobby_id) ||
        (association_.live && association_.lobby_id == lobby_id && association_.owner_user_id == sender)) {
        return;
    }
    std::uint64_t owner_user_id{};
    if (!read_lobby_owner(lobby_id, owner_user_id) || sender != owner_user_id) {
        return;
    }
    invalidate_association(AssociationEndReason::Replaced);
    start_association(lobby_id, owner_user_id, GetTickCount64());
}

void ClientTransport::on_native_disconnect(int physical_primary) noexcept {
    if (physical_primary == kHostPrimary && association_.live && on_network_thread("Disconnect client association")) {
        invalidate_association(AssociationEndReason::Disconnected);
    }
}

void ClientTransport::on_reset(std::uint8_t mode) noexcept {
    if (mode != 1 && association_.live && on_network_thread("Reset client associations")) {
        invalidate_association(AssociationEndReason::Reset);
    }
}

void ClientTransport::on_remote_member(const void* member_id, std::uint32_t state) noexcept {
    if (member_id == nullptr || (state & kMemberDeparture) == 0 || !association_.live ||
        !on_network_thread("Observe client lobby member")) {
        return;
    }
    std::uint64_t identity{};
    std::memcpy(&identity, member_id, sizeof(identity));
    if (identity == association_.owner_user_id) {
        invalidate_association(AssociationEndReason::HostChanged);
    }
}

void ClientTransport::on_local_lobby_left() noexcept {
    if (association_.live && on_network_thread("Leave client lobby")) {
        invalidate_association(AssociationEndReason::LobbyLeft);
    }
}

} // namespace fusioncutter::patches::direct_transport::client
