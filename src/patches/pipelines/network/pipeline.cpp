#include "pipeline.hpp"

#include "layout.hpp"

#include <WinSock2.h>
#include <Windows.h>

#include <atomic>
#include <chrono>
#include <utility>

namespace fusioncutter::patches::network_pipeline {
namespace {

using RawFunction = void(__cdecl*)();
using ReceiveFunction = void(__cdecl*)() noexcept;
using IntakeFunction = void(__fastcall*)(void*, void*) noexcept;
using DisconnectFunction = void(__fastcall*)(int) noexcept;
using ResetFunction = void(__fastcall*)(int) noexcept;

std::atomic<const TransportCallbacks*> gTransport;
std::atomic<const DiagnosticsCallbacks*> gDiagnostics;
OriginalFunction<RawFunction> gFinalSendOriginal;
OriginalFunction<RawFunction> gGroupSendOriginal;
OriginalFunction<ReceiveFunction> gReceiveOriginal;
OriginalFunction<IntakeFunction> gIntakeOriginal;
OriginalFunction<DisconnectFunction> gDisconnectOriginal;
OriginalFunction<ResetFunction> gResetOriginal;
OriginalFunction<ReceiveFunction> gClientReceiveOriginal;
thread_local int gTransmitGroupPrimary = -1;
thread_local bool gDirectIntake{};
std::uint64_t gClientReceiveSequence{};
#if defined(FC_NETWORK_PIPELINE_ABI_TEST)
RawFunction gFinalSendTestOriginal{};
RawFunction gGroupSendTestOriginal{};
ReceiveFunction gClientReceiveTestOriginal{};
#endif

[[nodiscard]] RawFunction final_send_original() noexcept {
#if defined(FC_NETWORK_PIPELINE_ABI_TEST)
    if (gFinalSendTestOriginal != nullptr) {
        return gFinalSendTestOriginal;
    }
#endif
    return gFinalSendOriginal.get();
}

[[nodiscard]] RawFunction group_send_original() noexcept {
#if defined(FC_NETWORK_PIPELINE_ABI_TEST)
    if (gGroupSendTestOriginal != nullptr) {
        return gGroupSendTestOriginal;
    }
#endif
    return gGroupSendOriginal.get();
}

[[nodiscard]] ReceiveFunction client_receive_original() noexcept {
#if defined(FC_NETWORK_PIPELINE_ABI_TEST)
    if (gClientReceiveTestOriginal != nullptr) {
        return gClientReceiveTestOriginal;
    }
#endif
    return gClientReceiveOriginal.get();
}

[[nodiscard]] std::uint64_t timestamp_ns() noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

// Restores the caller's Win32 and Winsock errors after patch callbacks return.
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

// Routes one final native packet through the active transport and records its carrier.
int __fastcall on_final_send(int destination, const void* bytes, int length, const void*) noexcept {
    const auto incoming_errors = LastErrors::capture();
    NativeSendResult transmit{};
    if (const auto* callbacks = gTransport.load(std::memory_order_acquire);
        callbacks != nullptr && bytes != nullptr && length > 0) {
        transmit = callbacks->send(callbacks->context, destination, gTransmitGroupPrimary,
                                   {static_cast<const std::uint8_t*>(bytes), static_cast<std::size_t>(length)});
    }

    auto result = transmit.result;
    auto carrier = transmit.carrier;
    if (!transmit.handled) {
        incoming_errors.restore();
        const auto original = final_send_original();
        result = original == nullptr ? -1 : call_final_send_original(destination, bytes, length, original);
        carrier = PacketCarrier::Native;
    } else {
        incoming_errors.restore();
        if (result < 0) {
            WSASetLastError(transmit.error);
        }
    }

    const auto outgoing_errors = LastErrors::capture();
    if (const auto* diagnostics = gDiagnostics.load(std::memory_order_acquire); diagnostics != nullptr) {
        diagnostics->send(diagnostics->context, destination, length > 0 ? static_cast<std::size_t>(length) : 0, carrier,
                          result);
    }
    outgoing_errors.restore();
    return result;
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
__declspec(naked) void __fastcall call_group_send_original(int, int, void*, RawFunction) {
    __asm {
        push dword ptr [esp + 4]
        call dword ptr [esp + 12]
        add  esp, 4
        ret  8
    }
}

// Pins one transport carrier while the game emits a native packet group.
void __fastcall on_group_send(int destination, int packet_type, void* group, const void*) noexcept {
    const auto incoming_errors = LastErrors::capture();
    const auto previous_group = gTransmitGroupPrimary;
    auto group_primary = -1;
    const auto* transport = gTransport.load(std::memory_order_acquire);
    if (transport != nullptr) {
        transport->native_transmit(transport->context, destination);
        group_primary = transport->begin_group(transport->context, destination, packet_type);
        gTransmitGroupPrimary = group_primary;
    }
    if (const auto* diagnostics = gDiagnostics.load(std::memory_order_acquire); diagnostics != nullptr) {
        diagnostics->group(diagnostics->context, destination, true);
    }

    incoming_errors.restore();
    if (const auto original = group_send_original(); original != nullptr) {
        call_group_send_original(destination, packet_type, group, original);
    }
    const auto outgoing_errors = LastErrors::capture();
    if (const auto* diagnostics = gDiagnostics.load(std::memory_order_acquire); diagnostics != nullptr) {
        diagnostics->group(diagnostics->context, destination, false);
    }
    if (transport != nullptr && group_primary >= 0) {
        transport->end_group(transport->context, group_primary);
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

// Pumps patch traffic around one native receive-service pass.
void __cdecl hook_receive() noexcept {
    const auto incoming_errors = LastErrors::capture();
    const auto* transport = gTransport.load(std::memory_order_acquire);
    const auto* diagnostics = gDiagnostics.load(std::memory_order_acquire);
    if (diagnostics != nullptr) {
        diagnostics->receive(diagnostics->context, true);
    }
    if (transport != nullptr) {
        transport->before_receive(transport->context);
    }
    incoming_errors.restore();
    if (const auto original = gReceiveOriginal.get(); original != nullptr) {
        original();
    }
    const auto outgoing_errors = LastErrors::capture();
    if (transport != nullptr) {
        transport->after_receive(transport->context);
    }
    if (diagnostics != nullptr) {
        diagnostics->receive(diagnostics->context, false);
    }
    outgoing_errors.restore();
}

// Publishes the exact client transaction that selects and applies complete server updates.
void __cdecl hook_client_receive() noexcept {
    const auto incoming_errors = LastErrors::capture();
    ClientReceiveBoundary boundary{
        .sequence = ++gClientReceiveSequence,
        .start_ns = timestamp_ns(),
    };
    const auto* transport = gTransport.load(std::memory_order_acquire);
    const auto* diagnostics = gDiagnostics.load(std::memory_order_acquire);
    if (diagnostics != nullptr && diagnostics->client_receive != nullptr) {
        diagnostics->client_receive(diagnostics->context, boundary, true);
    }
    if (transport != nullptr && transport->client_receive != nullptr) {
        transport->client_receive(transport->context, boundary, true);
    }

    incoming_errors.restore();
    if (const auto original = client_receive_original(); original != nullptr) {
        original();
    }
    const auto outgoing_errors = LastErrors::capture();
    boundary.completion_ns = timestamp_ns();
    if (transport != nullptr && transport->client_receive != nullptr) {
        transport->client_receive(transport->context, boundary, false);
    }
    if (diagnostics != nullptr && diagnostics->client_receive != nullptr) {
        diagnostics->client_receive(diagnostics->context, boundary, false);
    }
    outgoing_errors.restore();
}

// Preserves native intake before publishing the packet and endpoint observations.
void __fastcall hook_intake(void* packet, void* endpoint) noexcept {
    if (const auto original = gIntakeOriginal.get(); original != nullptr) {
        original(packet, endpoint);
    }
    const auto outgoing_errors = LastErrors::capture();
    if (const auto* transport = gTransport.load(std::memory_order_acquire); transport != nullptr) {
        transport->intake(transport->context, endpoint);
    }
    if (const auto* diagnostics = gDiagnostics.load(std::memory_order_acquire); diagnostics != nullptr) {
        diagnostics->intake(diagnostics->context, endpoint, gDirectIntake);
    }
    outgoing_errors.restore();
}

// Brackets disconnect so consumers can retire state at the correct native boundary.
void __fastcall hook_disconnect(int physical_primary) noexcept {
    const auto incoming_errors = LastErrors::capture();
    const auto* transport = gTransport.load(std::memory_order_acquire);
    const auto* diagnostics = gDiagnostics.load(std::memory_order_acquire);
    if (transport != nullptr) {
        transport->disconnect(transport->context, physical_primary);
    }
    if (diagnostics != nullptr) {
        diagnostics->disconnect(diagnostics->context, physical_primary, true);
    }
    incoming_errors.restore();
    if (const auto original = gDisconnectOriginal.get(); original != nullptr) {
        original(physical_primary);
    }
    const auto outgoing_errors = LastErrors::capture();
    if (transport != nullptr) {
        transport->disconnect_complete(transport->context, physical_primary);
    }
    if (diagnostics != nullptr) {
        diagnostics->disconnect(diagnostics->context, physical_primary, false);
    }
    outgoing_errors.restore();
}

// Retires patch state before the game performs its native network reset.
void __fastcall hook_reset(int mode) noexcept {
    const auto incoming_errors = LastErrors::capture();
    if (const auto* transport = gTransport.load(std::memory_order_acquire); transport != nullptr) {
        transport->reset(transport->context, static_cast<std::uint8_t>(mode));
    }
    if (const auto* diagnostics = gDiagnostics.load(std::memory_order_acquire); diagnostics != nullptr) {
        diagnostics->reset(diagnostics->context, static_cast<std::uint8_t>(mode));
    }
    incoming_errors.restore();
    if (const auto original = gResetOriginal.get(); original != nullptr) {
        original(mode);
    }
}

} // namespace

NetworkPipeline::NetworkPipeline(const TargetContext& target) noexcept : target_(target.layout), role_(target.role) {}

void NetworkPipeline::build_plan(PatchPlan& plan) {
    const auto& layout = layout_for(target_);
    gFinalSendOriginal = plan.inline_hook_with_original("Observe final online sends", layout.final_send_rva,
                                                        BytePattern::exact(kFinalSendPreimage),
                                                        reinterpret_cast<RawFunction>(&hook_final_send));
    gGroupSendOriginal = plan.inline_hook_with_original("Observe native packet groups", layout.group_send_rva,
                                                        BytePattern::exact(kGroupSendPreimage),
                                                        reinterpret_cast<RawFunction>(&hook_group_send));
    gReceiveOriginal = plan.inline_hook_with_original("Observe network receive service", layout.receive_rva,
                                                      BytePattern::exact(kReceivePreimage), &hook_receive);
    gIntakeOriginal = plan.inline_hook_with_original("Observe native packet intake", layout.intake_rva,
                                                     BytePattern::exact(kIntakePreimage), &hook_intake);
    gDisconnectOriginal = plan.inline_hook_with_original("Observe native disconnect", layout.disconnect_rva,
                                                         BytePattern::exact(kDisconnectPreimage), &hook_disconnect);
    gResetOriginal = plan.inline_hook_with_original("Observe native network reset", layout.reset_rva,
                                                    BytePattern::exact(kResetPreimage), &hook_reset);
    if (role_ == HostRole::Client) {
        gClientReceiveOriginal =
            plan.inline_hook_with_original("Observe client update drains", layout.client_receive_rva,
                                           BytePattern::exact(kClientReceivePreimage), &hook_client_receive);
    }
}

void publish_transport(const TransportCallbacks& callbacks) noexcept {
    gTransport.store(&callbacks, std::memory_order_release);
}

void clear_transport(const TransportCallbacks& callbacks) noexcept {
    auto* expected = &callbacks;
    static_cast<void>(gTransport.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel));
}

void publish_diagnostics(const DiagnosticsCallbacks& callbacks) noexcept {
    gDiagnostics.store(&callbacks, std::memory_order_release);
}

void clear_diagnostics(const DiagnosticsCallbacks& callbacks) noexcept {
    auto* expected = &callbacks;
    static_cast<void>(gDiagnostics.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel));
}

void observe_direct_association(const DirectAssociationObservation& observation) noexcept {
    if (const auto* diagnostics = gDiagnostics.load(std::memory_order_acquire);
        diagnostics != nullptr && diagnostics->direct_association != nullptr) {
        diagnostics->direct_association(diagnostics->context, observation);
    }
}

void observe_direct_receive(const DirectReceiveObservation& observation) noexcept {
    if (const auto* diagnostics = gDiagnostics.load(std::memory_order_acquire);
        diagnostics != nullptr && diagnostics->direct_receive != nullptr) {
        diagnostics->direct_receive(diagnostics->context, observation);
    }
}

void observe_output_pacing(const OutputPacingObservation& observation) noexcept {
    if (const auto* diagnostics = gDiagnostics.load(std::memory_order_acquire);
        diagnostics != nullptr && diagnostics->output_pacing != nullptr) {
        diagnostics->output_pacing(diagnostics->context, observation);
    }
}

void disconnect_native(int physical_primary) noexcept {
    const auto incoming_errors = LastErrors::capture();
    const auto* diagnostics = gDiagnostics.load(std::memory_order_acquire);
    if (diagnostics != nullptr) {
        diagnostics->disconnect(diagnostics->context, physical_primary, true);
    }
    incoming_errors.restore();
    if (const auto original = gDisconnectOriginal.get(); original != nullptr) {
        original(physical_primary);
    }
    const auto outgoing_errors = LastErrors::capture();
    if (diagnostics != nullptr) {
        diagnostics->disconnect(diagnostics->context, physical_primary, false);
    }
    outgoing_errors.restore();
}

void submit_native(void* packet, void* endpoint) noexcept {
    const auto previous = gDirectIntake;
    gDirectIntake = true;
    hook_intake(packet, endpoint);
    gDirectIntake = previous;
}

#if defined(FC_NETWORK_PIPELINE_ABI_TEST)
void configure_hooks_for_test(const TransportCallbacks& transport, const DiagnosticsCallbacks& diagnostics,
                              const HookOriginals& originals) noexcept {
    gFinalSendTestOriginal = reinterpret_cast<RawFunction>(originals.final_send);
    gGroupSendTestOriginal = reinterpret_cast<RawFunction>(originals.group_send);
    gClientReceiveTestOriginal = reinterpret_cast<ReceiveFunction>(originals.client_receive);
    publish_transport(transport);
    publish_diagnostics(diagnostics);
}

void* hook_for_test(HookPoint point) noexcept {
    switch (point) {
    case HookPoint::FinalSend:
        return reinterpret_cast<void*>(&hook_final_send);
    case HookPoint::GroupSend:
        return reinterpret_cast<void*>(&hook_group_send);
    case HookPoint::ClientReceive:
        return reinterpret_cast<void*>(&hook_client_receive);
    }
    std::unreachable();
}
#endif

} // namespace fusioncutter::patches::network_pipeline
