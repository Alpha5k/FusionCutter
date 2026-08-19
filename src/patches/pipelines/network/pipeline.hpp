#pragma once

#include <FusionCutter/patch.hpp>

#include <cstddef>
#include <cstdint>
#include <span>

namespace fusioncutter::patches::network_pipeline {

enum class PacketCarrier : std::uint8_t {
    Native,
    Direct,
    NotSent,
};

struct NativeSendResult {
    std::int32_t result;
    std::int32_t error;
    bool handled;
    PacketCarrier carrier{PacketCarrier::NotSent};
};

enum class DirectAssociationPhase : std::uint8_t {
    Started,
    Terminal,
    Ended,
};

struct DirectAssociationObservation {
    std::int32_t slot;
    std::uint32_t generation;
    std::uint32_t connection_id;
    DirectAssociationPhase phase;
    std::uint8_t route;
    std::uint8_t route_reason;
    std::uint8_t end_reason;
    std::uint32_t attempts;
    std::uint32_t tx_datagrams;
    std::uint32_t rx_datagrams;
    std::uint32_t send_failures;
    std::uint32_t endpoint_rejects;
    std::uint32_t authentication_rejects;
    std::uint32_t replay_rejects;
    std::uint32_t invalid_rejects;
    std::uint64_t elapsed_ms;
    std::uint64_t direct_ms;
};

struct DirectReceiveObservation {
    std::int32_t slot;
    std::uint32_t generation;
    std::uint32_t connection_id;
    std::uint32_t sequence;
};

enum class OutputPacingOutcome : std::uint8_t {
    Immediate = 1,
    Held = 2,
    CapLimited = 3,
    QueueCollision = 4,
    LifecycleDiscard = 5,
    ModeTransition = 6,
    CapacityExceeded = 7,
};

struct OutputPacingObservation {
    std::int32_t slot;
    std::uint32_t generation;
    OutputPacingOutcome outcome;
    std::uint16_t fragment_count;
    std::uint32_t bytes;
    std::uint64_t completion_ns;
    std::uint64_t release_ns;
};

// Identifies one native client transaction that selects and applies complete server updates.
struct ClientReceiveBoundary {
    std::uint64_t sequence;
    std::uint64_t start_ns;
    std::uint64_t completion_ns;
};

// Connects the shared game hooks to one role-specific transport implementation.
struct TransportCallbacks {
    void* context;
    void (*before_receive)(void*) noexcept;
    void (*after_receive)(void*) noexcept;
    void (*native_transmit)(void*, int) noexcept;
    int (*begin_group)(void*, int, int) noexcept;
    void (*end_group)(void*, int) noexcept;
    NativeSendResult (*send)(void*, int, int, std::span<const std::uint8_t>) noexcept;
    void (*intake)(void*, void*) noexcept;
    void (*disconnect)(void*, int) noexcept;
    void (*disconnect_complete)(void*, int) noexcept;
    void (*reset)(void*, std::uint8_t) noexcept;
    void (*client_receive)(void*, const ClientReceiveBoundary&, bool) noexcept;
};

// Supplies the fixed diagnostic observations emitted by the shared network hooks.
struct DiagnosticsCallbacks {
    void* context;
    void (*group)(void*, int, bool) noexcept;
    void (*send)(void*, int, std::size_t, PacketCarrier, int) noexcept;
    void (*receive)(void*, bool) noexcept;
    void (*intake)(void*, void*, bool) noexcept;
    void (*disconnect)(void*, int, bool) noexcept;
    void (*reset)(void*, std::uint8_t) noexcept;
    void (*direct_association)(void*, const DirectAssociationObservation&) noexcept;
    void (*direct_receive)(void*, const DirectReceiveObservation&) noexcept;
    void (*output_pacing)(void*, const OutputPacingObservation&) noexcept;
    void (*client_receive)(void*, const ClientReceiveBoundary&, bool) noexcept;
};

void publish_transport(const TransportCallbacks& callbacks) noexcept;
void clear_transport(const TransportCallbacks& callbacks) noexcept;
void publish_diagnostics(const DiagnosticsCallbacks& callbacks) noexcept;
void clear_diagnostics(const DiagnosticsCallbacks& callbacks) noexcept;
void observe_direct_association(const DirectAssociationObservation& observation) noexcept;
void observe_direct_receive(const DirectReceiveObservation& observation) noexcept;
void observe_output_pacing(const OutputPacingObservation& observation) noexcept;

// Re-enters the preserved native disconnect path for a policy-driven removal.
void disconnect_native(int physical_primary) noexcept;
// Delivers a reconstructed direct packet through the preserved native intake path.
void submit_native(void* packet, void* endpoint) noexcept;

class NetworkPipeline final : public Patch {
  public:
    explicit NetworkPipeline(const TargetContext& target) noexcept;

    // Installs the common native send, receive, intake, disconnect, and reset hooks.
    void build_plan(PatchPlan& plan) override;

  private:
    TargetLayout target_;
    HostRole role_;
};

#if defined(FC_NETWORK_PIPELINE_ABI_TEST)
enum class HookPoint {
    FinalSend,
    GroupSend,
    ClientReceive,
};

struct HookOriginals {
    void* final_send;
    void* group_send;
    void* client_receive;
};

void configure_hooks_for_test(const TransportCallbacks& transport, const DiagnosticsCallbacks& diagnostics,
                              const HookOriginals& originals) noexcept;
[[nodiscard]] void* hook_for_test(HookPoint point) noexcept;
#endif

} // namespace fusioncutter::patches::network_pipeline
