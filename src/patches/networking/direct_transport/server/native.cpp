#include "transport.hpp"

#include "../shared/datagram.hpp"

#include <WinSock2.h>
#include <Windows.h>

#include <cstring>

namespace fusioncutter::patches::direct_transport::server {
namespace {

constexpr std::uint32_t kMemberDeparture = 0x02 | 0x04 | 0x08 | 0x10;

} // namespace

void ServerTransport::before_receive() noexcept {
    if (!claim_network_thread("Pump server network receive")) {
        return;
    }
    const auto now = GetTickCount64();
    pump_control(now);
    pump_direct();
}

void ServerTransport::after_receive() noexcept {
    if (on_network_thread("Service server associations")) {
        service_associations(GetTickCount64());
    }
}

void ServerTransport::on_native_transmit(int physical_primary) noexcept {
    if (!on_network_thread("Observe server native transmit")) {
        return;
    }
    const auto resolved = resolve_primary(physical_primary);
    if (resolved < 0 || rearm_blocked_[resolved]) {
        return;
    }
    std::uint64_t identity{};
    if (!read_identity(static_cast<std::uint8_t>(resolved), identity)) {
        return;
    }
    auto& association = associations_[resolved];
    if (association.live && association.galaxy_id == identity) {
        return;
    }
    invalidate_association(static_cast<std::uint8_t>(resolved), AssociationEndReason::Replaced);
    start_association(static_cast<std::uint8_t>(resolved), identity, GetTickCount64());
}

int ServerTransport::begin_transmit_group(int destination) noexcept {
    if (!on_network_thread("Begin server transmit group")) {
        return -1;
    }
    const auto resolved = resolve_primary(destination);
    if (resolved < 0 || !associations_[resolved].live) {
        return -1;
    }
    auto& association = associations_[resolved];
    if (!association.transmit_group.active() && association.transmit_route == TransmitRoute::DirectPending) {
        set_route(static_cast<std::uint8_t>(resolved), association.state, TransmitRoute::Direct,
                  association.receive_permission);
    }
    const auto carrier = association.transmit_route == TransmitRoute::Direct ? Carrier::Direct : Carrier::Galaxy;
    if (!association.transmit_group.begin(association.generation, carrier)) {
        runtime_log_.error("The server transmit-group depth overflowed", "Begin server transmit group");
        return -1;
    }
    return resolved;
}

void ServerTransport::end_transmit_group(int physical_primary) noexcept {
    if (physical_primary < 0 || physical_primary >= static_cast<int>(associations_.size())) {
        return;
    }
    auto& association = associations_[physical_primary];
    if (!on_network_thread("End server transmit group")) {
        return;
    }
    if (!association.transmit_group.end()) {
        runtime_log_.error("A server transmit group ended without a matching begin", "End server transmit group");
    }
}

NativeTransmitResult ServerTransport::transmit_native(int destination, int group_primary,
                                                      std::span<const std::uint8_t> bytes) noexcept {
    if (!on_network_thread("Transmit server native packet")) {
        return {.result = -1, .error = WSAENOTCONN, .handled = true};
    }
    const auto resolved = group_primary >= 0 ? group_primary : resolve_primary(destination);
    if (resolved < 0 || resolved >= static_cast<int>(associations_.size())) {
        return {};
    }
    auto& association = associations_[resolved];
    const auto grouped = association.transmit_group.active();
    std::uint64_t identity{};
    const auto stale_group = grouped && !association.transmit_group.belongs_to(association.generation);
    if (stale_group) {
        runtime_log_.error("A server transmit group belongs to a stale association generation",
                           "Transmit server native packet");
    }
    const auto stale = !association.live || !read_identity(static_cast<std::uint8_t>(resolved), identity) ||
                       identity != association.galaxy_id || stale_group;
    if (stale) {
        return grouped || association.transmit_route != TransmitRoute::Galaxy
                   ? NativeTransmitResult{.result = -1, .error = WSAENOTCONN, .handled = true}
                   : NativeTransmitResult{};
    }

    auto carrier = grouped ? association.transmit_group.carrier() : Carrier::Galaxy;
    if (!grouped) {
        if (association.transmit_route == TransmitRoute::DirectPending) {
            set_route(static_cast<std::uint8_t>(resolved), association.state, TransmitRoute::Direct,
                      association.receive_permission);
        }
        if (association.transmit_route == TransmitRoute::Direct) {
            carrier = Carrier::Direct;
        }
    }
    if (carrier != Carrier::Direct) {
        return {};
    }
    if (association.state != RouteState::DirectLocked || !association.committed_endpoint.valid() ||
        !socket_.available()) {
        runtime_log_.error("The server selected Direct without a usable transport", "Validate server direct route");
        return {.result = -1, .error = WSAENOTCONN, .handled = true};
    }
    const auto result =
        send_direct_data(socket_, send_buffer_, Direction::ServerToClient, association.session_key,
                         association.connection_id, association.send_sequence++, association.committed_endpoint, bytes);
    if (result.result < 0) {
        association.diagnostics.send_failed();
        if (result.failure == NativeTransmitResult::Failure::Serialization) {
            runtime_log_.error("The native game packet could not be serialized within protocol bounds",
                               "Serialize direct game data");
        } else if (result.failure == NativeTransmitResult::Failure::PartialSend) {
            runtime_log_.error("The UDP socket reported a partial datagram send", "Complete direct UDP send");
        } else {
            runtime_log_.socket("Send direct UDP data", result.error, is_internal_socket_error(result.error));
        }
    } else {
        association.diagnostics.direct_tx("Server", static_cast<std::uint8_t>(resolved), association.generation,
                                          association.connection_id);
    }
    return result;
}

void ServerTransport::on_native_intake(void* endpoint) noexcept {
    if (!on_network_thread("Observe server native intake")) {
        return;
    }
    const auto physical_primary = find_endpoint(endpoint);
    if (physical_primary < 0 || rearm_blocked_[physical_primary]) {
        return;
    }
    std::uint64_t identity{};
    if (!read_identity(static_cast<std::uint8_t>(physical_primary), identity)) {
        return;
    }
    auto& association = associations_[physical_primary];
    if (association.live && association.galaxy_id == identity) {
        return;
    }
    invalidate_association(static_cast<std::uint8_t>(physical_primary), AssociationEndReason::Replaced);
    start_association(static_cast<std::uint8_t>(physical_primary), identity, GetTickCount64());
}

void ServerTransport::on_native_disconnect(int physical_primary) noexcept {
    if (physical_primary >= 0 && physical_primary < static_cast<int>(associations_.size()) &&
        on_network_thread("Disconnect server association")) {
        rearm_blocked_[physical_primary] = true;
        invalidate_association(static_cast<std::uint8_t>(physical_primary), AssociationEndReason::Disconnected);
    }
}

void ServerTransport::on_native_disconnect_complete(int physical_primary) noexcept {
    if (physical_primary >= 0 && physical_primary < static_cast<int>(rearm_blocked_.size()) &&
        on_network_thread("Complete server disconnect")) {
        rearm_blocked_[physical_primary] = false;
    }
}

void ServerTransport::on_reset(std::uint8_t mode) noexcept {
    if (mode != 1 && on_network_thread("Reset server associations")) {
        invalidate_all(AssociationEndReason::Reset);
        pending_removals_.fill(-1);
        has_pending_removals_ = false;
    }
}

void ServerTransport::on_remote_member(const void* member_id, std::uint32_t state) noexcept {
    if (member_id == nullptr || (state & kMemberDeparture) == 0 || !on_network_thread("Observe server lobby member")) {
        return;
    }
    std::uint64_t identity{};
    std::memcpy(&identity, member_id, sizeof(identity));
    const auto physical_primary = find_identity(identity);
    if (physical_primary >= 0) {
        rearm_blocked_[physical_primary] = true;
        invalidate_association(static_cast<std::uint8_t>(physical_primary), AssociationEndReason::Disconnected);
    }
}

void ServerTransport::on_local_lobby_left() noexcept {
    if (on_network_thread("Leave server lobby")) {
        invalidate_all(AssociationEndReason::LobbyLeft);
        pending_removals_.fill(-1);
        has_pending_removals_ = false;
    }
}

} // namespace fusioncutter::patches::direct_transport::server
