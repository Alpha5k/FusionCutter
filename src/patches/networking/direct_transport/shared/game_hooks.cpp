#include "game_hooks.hpp"

#include <WinSock2.h>
#include <Windows.h>

#include <cstdint>

namespace fusioncutter::patches::direct_transport {
namespace {

using RawFunction = void(__cdecl*)();
using ReceiveFunction = void(__cdecl*)() noexcept;
using IntakeFunction = void(__fastcall*)(void*, void*) noexcept;
using DisconnectFunction = void(__fastcall*)(int) noexcept;
using ResetFunction = void(__fastcall*)(int) noexcept;
using RemoteMemberFunction = void(__thiscall*)(void*, const void*, const void*, std::uint32_t);
using LocalLobbyLeftFunction = void(__thiscall*)(void*, const void*, std::uint32_t);

PatchInstanceSlot<GameTransport> gActive;
OriginalFunction<RawFunction> gFinalSendOriginal;
OriginalFunction<RawFunction> gGroupSendOriginal;
OriginalFunction<ReceiveFunction> gReceiveOriginal;
OriginalFunction<IntakeFunction> gIntakeOriginal;
OriginalFunction<DisconnectFunction> gDisconnectOriginal;
OriginalFunction<ResetFunction> gResetOriginal;
RemoteMemberFunction gRemoteMemberOriginal{};
LocalLobbyLeftFunction gLocalLobbyLeftOriginal{};
thread_local int gTransmitGroupPrimary = -1;
#if defined(FC_DIRECT_TRANSPORT_ABI_TEST)
RawFunction gFinalSendTestOriginal{};
RawFunction gGroupSendTestOriginal{};
#endif

[[nodiscard]] RawFunction final_send_original() noexcept {
#if defined(FC_DIRECT_TRANSPORT_ABI_TEST)
    if (gFinalSendTestOriginal != nullptr) {
        return gFinalSendTestOriginal;
    }
#endif
    return gFinalSendOriginal.get();
}

[[nodiscard]] RawFunction group_send_original() noexcept {
#if defined(FC_DIRECT_TRANSPORT_ABI_TEST)
    if (gGroupSendTestOriginal != nullptr) {
        return gGroupSendTestOriginal;
    }
#endif
    return gGroupSendOriginal.get();
}

// Preserves caller-visible Win32 and Winsock errors while patch callbacks run.
struct LastErrors {
    DWORD windows;
    int sockets;

    [[nodiscard]] static LastErrors capture() noexcept {
        const auto windows = GetLastError();
        const auto sockets = WSAGetLastError();
        return {windows, sockets};
    }

    void restore() const noexcept {
        WSASetLastError(sockets);
        SetLastError(windows);
    }
};

// Adapts the final-send routine's register and caller-clean stack arguments.
__declspec(naked) int __fastcall call_final_send_original(int, const void*, int, RawFunction) {
    __asm {
        push dword ptr [esp + 4]
        call dword ptr [esp + 12]
        add  esp, 4
        ret  8
    }
}

// Routes one final native packet through Direct or falls back to the preserved Galaxy send.
int __fastcall on_final_send(int destination, const void* bytes, int length, const void*) noexcept {
    const auto incoming_errors = LastErrors::capture();
    NativeTransmitResult transmit{};
    if (auto* transport = gActive.read(); transport != nullptr && bytes != nullptr && length > 0) {
        transmit =
            transport->transmit_native(destination, gTransmitGroupPrimary,
                                       {static_cast<const std::uint8_t*>(bytes), static_cast<std::size_t>(length)});
    }
    if (!transmit.handled) {
        incoming_errors.restore();
        const auto original = final_send_original();
        return original == nullptr ? -1 : call_final_send_original(destination, bytes, length, original);
    }

    incoming_errors.restore();
    if (transmit.result < 0) {
        WSASetLastError(transmit.error);
    }
    return transmit.result;
}

// Transfers the native final-send entry state to the typed routing callback.
__declspec(naked) int hook_final_send() {
    __asm {
        push dword ptr [esp]
        push dword ptr [esp + 8]
        call on_final_send
        ret
    }
}

// Adapts the group-send routine's register and caller-clean stack arguments.
__declspec(naked) void __fastcall call_group_send_original(int, void*, void*, RawFunction) {
    __asm {
        push dword ptr [esp + 4]
        call dword ptr [esp + 12]
        add  esp, 4
        ret  8
    }
}

// Pins one carrier while the game emits all packets belonging to a native group.
void __fastcall on_group_send(int destination, void* native_argument, void* group, const void*) noexcept {
    const auto incoming_errors = LastErrors::capture();
    const auto previous_group = gTransmitGroupPrimary;
    auto group_primary = -1;
    if (auto* transport = gActive.read(); transport != nullptr) {
        transport->on_native_transmit(destination);
        group_primary = transport->begin_transmit_group(destination);
        gTransmitGroupPrimary = group_primary;
    }

    incoming_errors.restore();
    if (const auto original = group_send_original(); original != nullptr) {
        call_group_send_original(destination, native_argument, group, original);
    }
    const auto outgoing_errors = LastErrors::capture();
    if (auto* transport = gActive.read(); transport != nullptr && group_primary >= 0) {
        transport->end_transmit_group(group_primary);
    }
    gTransmitGroupPrimary = previous_group;
    outgoing_errors.restore();
}

// Transfers the native group-send entry state to the typed routing callback.
__declspec(naked) void hook_group_send() {
    __asm {
        push dword ptr [esp]
        push dword ptr [esp + 8]
        call on_group_send
        ret
    }
}

// Pumps patch traffic before receive and advances negotiation after the native pass.
void __cdecl hook_receive() noexcept {
    const auto incoming_errors = LastErrors::capture();
    if (auto* transport = gActive.read(); transport != nullptr) {
        transport->before_receive();
    }
    incoming_errors.restore();
    if (const auto original = gReceiveOriginal.get(); original != nullptr) {
        original();
    }
    const auto outgoing_errors = LastErrors::capture();
    if (auto* transport = gActive.read(); transport != nullptr) {
        transport->after_receive();
    }
    outgoing_errors.restore();
}

// Preserves native packet intake, then observes the endpoint for association discovery.
void __fastcall hook_intake(void* packet, void* endpoint) noexcept {
    if (const auto original = gIntakeOriginal.get(); original != nullptr) {
        original(packet, endpoint);
    }
    const auto outgoing_errors = LastErrors::capture();
    if (auto* transport = gActive.read(); transport != nullptr) {
        transport->on_native_intake(endpoint);
    }
    outgoing_errors.restore();
}

// Brackets native disconnect so an association cannot rearm until cleanup completes.
void __fastcall hook_disconnect(int physical_primary) noexcept {
    const auto incoming_errors = LastErrors::capture();
    if (auto* transport = gActive.read(); transport != nullptr) {
        transport->on_native_disconnect(physical_primary);
    }
    incoming_errors.restore();
    if (const auto original = gDisconnectOriginal.get(); original != nullptr) {
        original(physical_primary);
    }
    const auto outgoing_errors = LastErrors::capture();
    if (auto* transport = gActive.read(); transport != nullptr) {
        transport->on_native_disconnect_complete(physical_primary);
    }
    outgoing_errors.restore();
}

// Ends affected transport state before the game performs its network reset.
void __fastcall hook_reset(int mode) noexcept {
    const auto incoming_errors = LastErrors::capture();
    if (auto* transport = gActive.read(); transport != nullptr) {
        transport->on_reset(static_cast<std::uint8_t>(mode));
    }
    incoming_errors.restore();
    if (const auto original = gResetOriginal.get(); original != nullptr) {
        original(mode);
    }
}

// Ends associations for departing remote lobby members before forwarding the callback.
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

// Ends all local lobby associations before forwarding the departure callback.
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

GameHooks::GameHooks(ImageContext image, const GameLayout& layout) noexcept : image_(image), layout_(layout) {}

void GameHooks::build_plan(PatchPlan& plan) {
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

    gFinalSendOriginal = plan.inline_hook_with_original("Route final online sends", layout_.final_send_rva,
                                                        BytePattern::exact(kFinalSendPreimage),
                                                        reinterpret_cast<RawFunction>(&hook_final_send));
    gGroupSendOriginal = plan.inline_hook_with_original("Hold one carrier per native group", layout_.group_send_rva,
                                                        BytePattern::exact(kGroupSendPreimage),
                                                        reinterpret_cast<RawFunction>(&hook_group_send));
    gReceiveOriginal = plan.inline_hook_with_original("Pump Direct Transport receives", layout_.receive_rva,
                                                      BytePattern::exact(kReceivePreimage), &hook_receive);
    gIntakeOriginal = plan.inline_hook_with_original("Observe native packet intake", layout_.intake_rva,
                                                     BytePattern::exact(kIntakePreimage), &hook_intake);
    gDisconnectOriginal = plan.inline_hook_with_original("End disconnected associations", layout_.disconnect_rva,
                                                         BytePattern::exact(kDisconnectPreimage), &hook_disconnect);
    gResetOriginal = plan.inline_hook_with_original("End reset associations", layout_.reset_rva,
                                                    BytePattern::exact(kResetPreimage), &hook_reset);

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

void GameHooks::enable(GameTransport& transport) noexcept {
    gActive.publish(transport);
}

void GameHooks::disable(GameTransport& transport) noexcept {
    gActive.clear(transport);
}

void disconnect_native(int physical_primary) noexcept {
    if (const auto original = gDisconnectOriginal.get(); original != nullptr) {
        original(physical_primary);
    }
}

void submit_direct_native(void* packet, void* endpoint) noexcept {
    hook_intake(packet, endpoint);
}

#if defined(FC_DIRECT_TRANSPORT_ABI_TEST)
void configure_game_hooks_for_test(GameTransport& transport, const GameHookOriginals& originals) noexcept {
    gFinalSendTestOriginal = reinterpret_cast<RawFunction>(originals.final_send);
    gGroupSendTestOriginal = reinterpret_cast<RawFunction>(originals.group_send);
    gActive.publish(transport);
}

void* game_hook_for_test(GameHookPoint point) noexcept {
    return point == GameHookPoint::FinalSend ? reinterpret_cast<void*>(&hook_final_send)
                                             : reinterpret_cast<void*>(&hook_group_send);
}
#endif

} // namespace fusioncutter::patches::direct_transport
