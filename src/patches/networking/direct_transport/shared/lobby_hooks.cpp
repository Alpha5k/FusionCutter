#include "lobby_hooks.hpp"

#include <WinSock2.h>
#include <Windows.h>

#include <cstdint>

namespace fusioncutter::patches::direct_transport {
namespace {

using RemoteMemberFunction = void(__thiscall*)(void*, const void*, const void*, std::uint32_t);
using LocalLobbyLeftFunction = void(__thiscall*)(void*, const void*, std::uint32_t);

PatchInstanceSlot<GameTransport> gActive;
RemoteMemberFunction gRemoteMemberOriginal{};
LocalLobbyLeftFunction gLocalLobbyLeftOriginal{};

struct LastErrors {
    DWORD windows;
    int sockets;

    [[nodiscard]] static LastErrors capture() noexcept {
        return {GetLastError(), WSAGetLastError()};
    }

    void restore() const noexcept {
        WSASetLastError(sockets);
        SetLastError(windows);
    }
};

// Retires associations for a departing remote member before forwarding the lobby callback.
void __fastcall hook_remote_member(void* self, void*, const void* lobby_id, const void* member_id,
                                   std::uint32_t state) noexcept {
    const auto incoming_errors = LastErrors::capture();
    if (auto* transport = gActive.read(); transport != nullptr) {
        transport->on_remote_member(member_id, state);
    }
    incoming_errors.restore();
    if (gRemoteMemberOriginal != nullptr) {
        gRemoteMemberOriginal(self, lobby_id, member_id, state);
    }
}

// Retires local lobby associations before forwarding the departure callback.
void __fastcall hook_local_lobby_left(void* self, void*, const void* lobby_id, std::uint32_t reason) noexcept {
    const auto incoming_errors = LastErrors::capture();
    if (auto* transport = gActive.read(); transport != nullptr) {
        transport->on_local_lobby_left();
    }
    incoming_errors.restore();
    if (gLocalLobbyLeftOriginal != nullptr) {
        gLocalLobbyLeftOriginal(self, lobby_id, reason);
    }
}

} // namespace

LobbyHooks::LobbyHooks(ImageContext image, const GameLayout& layout) noexcept : image_(image), layout_(layout) {}

void LobbyHooks::build_plan(PatchPlan& plan) {
    const auto matchmaking = relocated_getter_preimage(layout_, image_.base, false);
    const auto networking = relocated_getter_preimage(layout_, image_.base, true);
    const auto allocate = relocated_packet_preimage(layout_, image_.base, false);
    const auto initialize = relocated_packet_preimage(layout_, image_.base, true);
    plan.require_bytes("Validate Galaxy matchmaking access", layout_.get_matchmaking_rva,
                       BytePattern::exact(matchmaking));
    plan.require_bytes("Validate Galaxy networking access", layout_.get_networking_rva, BytePattern::exact(networking));
    plan.require_bytes("Validate native packet allocation", layout_.packet_allocate_rva, BytePattern::exact(allocate));
    plan.require_bytes("Validate native packet initialization", layout_.packet_initialize_rva,
                       BytePattern::exact(initialize));
    plan.require_bytes("Validate remote lobby callback", layout_.remote_member_callback_rva,
                       BytePattern::exact(kRemoteMemberCallbackPreimage));
    plan.require_bytes("Validate local lobby callback", layout_.local_lobby_left_callback_rva,
                       BytePattern::exact(kLocalLobbyLeftCallbackPreimage));

    gRemoteMemberOriginal = image_.function_at_rva<RemoteMemberFunction>(layout_.remote_member_callback_rva);
    gLocalLobbyLeftOriginal = image_.function_at_rva<LocalLobbyLeftFunction>(layout_.local_lobby_left_callback_rva);
    const auto remote_expected = static_cast<std::uint32_t>(image_.address_at_rva(layout_.remote_member_callback_rva));
    const auto local_expected =
        static_cast<std::uint32_t>(image_.address_at_rva(layout_.local_lobby_left_callback_rva));
    plan.checked_write("Observe remote lobby departures", layout_.remote_member_listener_rva, remote_expected,
                       PatchAddress::absolute(&hook_remote_member));
    plan.checked_write("Observe local lobby departure", layout_.local_lobby_left_listener_rva, local_expected,
                       PatchAddress::absolute(&hook_local_lobby_left));
}

void LobbyHooks::enable(GameTransport& transport) noexcept {
    gActive.publish(transport);
}

void LobbyHooks::disable(GameTransport& transport) noexcept {
    gActive.clear(transport);
}

} // namespace fusioncutter::patches::direct_transport
