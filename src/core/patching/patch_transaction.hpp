#pragma once

#include "native_allocation.hpp"
#include "native_memory.hpp"

#include "../planning/planning_types.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

namespace fc::patching {

// Rollback records whether native work was never visible, was exactly reversed, or leaves enumerated effects.
enum class RollbackResult {
    NoExposure,
    Restored,
    Residual,
};

// A Commit phase failure carries the reported reason and containment result required for lifecycle ownership.
struct CommitFailure {
    planning::FailureReason reason;
    RollbackResult rollback{RollbackResult::NoExposure};
    // False means protection or ownership coherence was lost and the caller must take the fatal path after retention.
    bool contained{true};
};

// Prepared effects cover vendor-managed hook publication whose exact target bytes remain owned by that vendor.
using NativeEffectFunction = std::expected<void, NativeWriteFailure> (*)(void* context);

struct NativeCommitEffect {
    void* context{};
    NativeEffectFunction apply{};
    NativeEffectFunction restore{};
};

// Owns the prepared native write sequence, its exact undo records, and all resources referenced by those writes.
class PatchTransaction final {
  public:
    explicit PatchTransaction(NativeMemoryWriter writer = system_memory_writer()) noexcept;
    PatchTransaction(const PatchTransaction&) = delete;
    PatchTransaction& operator=(const PatchTransaction&) = delete;
    PatchTransaction(PatchTransaction&&) noexcept = default;
    PatchTransaction& operator=(PatchTransaction&&) noexcept = default;

    // Preparation records final writes and every allocation those writes may expose, but changes no target memory.
    [[nodiscard]] std::expected<void, std::string> add_write(std::uintptr_t address,
                                                             std::span<const std::byte> replacement,
                                                             std::uint32_t operation_index, std::string operation);
    [[nodiscard]] std::expected<void, std::string> add_effect(NativeCommitEffect effect, std::string operation);
    void add_data_allocation(FC_DataHandle handle, NativeAllocation allocation);
    void add_relay_allocation(NativeAllocation allocation);

    // Symbolic resolution is callback-scoped, while the returned allocation remains owned by this transaction.
    [[nodiscard]] bool resolve_data(FC_DataHandle handle, std::uintptr_t& address,
                                    std::uint64_t& byte_size) const noexcept;
    // Diagnostic publication may inspect, but never take, resources still retained after an exposed failure.
    [[nodiscard]] const NativePatchResources& resources() const noexcept;

    // Captures the complete undo state, applies writes in order, and classifies any reverse unwind before returning.
    [[nodiscard]] std::expected<NativePatchResources, CommitFailure> commit();
    // Drops unpublished writes and allocations after the plugin instance has stopped using prepared storage.
    void release_prepared_resources() noexcept;

  private:
    // Prepared and undo records keep publication order separate from state used for restoration in reverse order.
    struct PreparedWrite {
        std::uintptr_t address{};
        std::vector<std::byte> replacement;
        // Retain the planned operation's ordinal for crash annotations; the Commit phase uses its display name.
        std::uint32_t operation_index{};
        std::string operation;
    };

    struct UndoWrite {
        std::uintptr_t address{};
        std::vector<std::byte> original;
        bool applied{};
    };

    // Effects retain their owner by pointer for this attempt; HookPreparation owns it through the Commit phase.
    struct PreparedEffect {
        NativeCommitEffect effect;
        std::string operation;
        bool applied{};
    };

    struct RollbackOutcome {
        RollbackResult result{RollbackResult::NoExposure};
        bool contained{true};
    };

    [[nodiscard]] RollbackOutcome rollback() noexcept;

    NativeMemoryWriter writer_;
    std::vector<PreparedWrite> writes_;
    std::vector<UndoWrite> undo_;
    std::vector<PreparedEffect> effects_;
    NativePatchResources resources_;
};

} // namespace fc::patching
