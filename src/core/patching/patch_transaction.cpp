#include "patch_transaction.hpp"

#include <limits>
#include <utility>

namespace fc::patching {
namespace {

[[nodiscard]] planning::FailureReason commit_reason(std::string message, const std::string& operation) {
    return {.message = std::move(message),
            .phase = planning::PatchPhase::Commit,
            .operation = operation.empty() ? std::optional<std::string>{"Commit native operation"}
                                           : std::optional<std::string>{operation}};
}

} // namespace

PatchTransaction::PatchTransaction(NativeMemoryWriter writer) noexcept : writer_(writer) {}

std::expected<void, std::string> PatchTransaction::add_write(std::uintptr_t address,
                                                             std::span<const std::byte> replacement,
                                                             std::uint32_t operation_index, std::string operation) {
    if (writer_.write == nullptr || address == 0 || replacement.empty() ||
        address > std::numeric_limits<std::uintptr_t>::max() - replacement.size()) {
        return std::unexpected("Prepared native write has an invalid range or writer");
    }
    writes_.push_back({address, {replacement.begin(), replacement.end()}, operation_index, std::move(operation)});
    return {};
}

std::expected<void, std::string> PatchTransaction::add_effect(NativeCommitEffect effect, std::string operation) {
    if (effect.context == nullptr || effect.apply == nullptr || effect.restore == nullptr) {
        return std::unexpected("Prepared native effect has an invalid owner or callback");
    }
    effects_.push_back({effect, std::move(operation)});
    return {};
}

void PatchTransaction::add_data_allocation(FC_DataHandle handle, NativeAllocation allocation) {
    resources_.data_allocations.push_back({handle, std::move(allocation)});
}

void PatchTransaction::add_relay_allocation(NativeAllocation allocation) {
    resources_.relay_allocations.push_back(std::move(allocation));
}

bool PatchTransaction::resolve_data(FC_DataHandle handle, std::uintptr_t& address,
                                    std::uint64_t& byte_size) const noexcept {
    for (const auto& allocation : resources_.data_allocations) {
        if (allocation.handle == handle) {
            address = allocation.allocation.address();
            byte_size = allocation.allocation.size();
            return true;
        }
    }
    address = 0;
    byte_size = 0;
    return false;
}

const NativePatchResources& PatchTransaction::resources() const noexcept {
    return resources_;
}

std::expected<NativePatchResources, CommitFailure> PatchTransaction::commit() {
    // Capture and allocate the complete undo history before any replacement can become visible.
    undo_.clear();
    try {
        undo_.reserve(writes_.size());
        for (const auto& write : writes_) {
            auto original = read_native_memory(write.address, write.replacement.size());
            if (!original) {
                return std::unexpected(CommitFailure{commit_reason(original.error(), write.operation)});
            }
            undo_.push_back({write.address, std::move(*original)});
        }
    } catch (...) {
        undo_.clear();
        return std::unexpected(CommitFailure{commit_reason(
            "The Commit phase could not allocate its complete undo history", "Capture native preimages")});
    }

    // Publish in the order of planned operations; failure unwinds every range whose replacement bytes may be visible.
    for (std::size_t index = 0; index < writes_.size(); ++index) {
        const auto& write = writes_[index];
        std::expected<void, NativeWriteFailure> applied;
        try {
            applied = writer_.write(writer_.context, write.address, write.replacement);
        } catch (...) {
            // Unknown adapter exposure is bounded to this prepared range; restore it and force the fatal path.
            undo_[index].applied = true;
            const auto unwind = rollback();
            return std::unexpected(
                CommitFailure{commit_reason("Native memory writer threw during the Commit phase", write.operation),
                              unwind.result, false});
        }
        if (applied) {
            undo_[index].applied = true;
            continue;
        }
        // A failed platform write may still have exposed a prefix, so its exact preimage joins reverse unwind.
        undo_[index].applied = applied.error().changed;
        const auto unwind = rollback();
        return std::unexpected(CommitFailure{commit_reason(std::move(applied.error().message), write.operation),
                                             unwind.result, applied.error().contained && unwind.contained});
    }

    // Vendor-managed effects publish after ordinary bytes so reverse unwind can close them before restoring inputs.
    for (auto& effect : effects_) {
        std::expected<void, NativeWriteFailure> applied;
        try {
            applied = effect.effect.apply(effect.effect.context);
        } catch (...) {
            // An escaping native adapter cannot state whether it exposed bytes, so retention must assume it did.
            effect.applied = true;
            const auto unwind = rollback();
            return std::unexpected(CommitFailure{
                commit_reason("Native effect threw during the Commit phase", effect.operation), unwind.result, false});
        }
        if (applied) {
            effect.applied = true;
            continue;
        }
        effect.applied = applied.error().changed;
        const auto unwind = rollback();
        return std::unexpected(CommitFailure{commit_reason(std::move(applied.error().message), effect.operation),
                                             unwind.result, applied.error().contained && unwind.contained});
    }

    // Successful native state has no uninstall surface; process-lifetime owners receive only its resources.
    undo_.clear();
    writes_.clear();
    effects_.clear();
    return std::move(resources_);
}

void PatchTransaction::release_prepared_resources() noexcept {
    writes_.clear();
    undo_.clear();
    effects_.clear();
    resources_ = {};
}

PatchTransaction::RollbackOutcome PatchTransaction::rollback() noexcept {
    bool exposed{};
    bool restored = true;
    bool contained = true;
    // Close effects before restoring bytes they may call through or otherwise reference.
    for (auto iterator = effects_.rbegin(); iterator != effects_.rend(); ++iterator) {
        if (!iterator->applied) {
            continue;
        }
        exposed = true;
        try {
            auto result = iterator->effect.restore(iterator->effect.context);
            if (!result) {
                restored = false;
                contained = contained && result.error().contained;
            } else {
                iterator->applied = false;
            }
        } catch (...) {
            restored = false;
            contained = false;
        }
    }
    // Reverse publication order preserves the semantics of overlapping writes and restores the original image bytes.
    for (auto iterator = undo_.rbegin(); iterator != undo_.rend(); ++iterator) {
        if (!iterator->applied) {
            continue;
        }
        exposed = true;
        try {
            auto result = writer_.write(writer_.context, iterator->address, iterator->original);
            if (!result) {
                restored = false;
                contained = contained && result.error().contained;
            } else {
                iterator->applied = false;
            }
        } catch (...) {
            // The remaining ranges are still known, but an injected or platform adapter that unwinds is not coherent.
            restored = false;
            contained = false;
        }
    }
    // Separate NoExposure, Restored, and Residual because installation assigns each a different severity.
    if (!exposed) {
        return {RollbackResult::NoExposure, contained};
    }
    return {restored ? RollbackResult::Restored : RollbackResult::Residual, contained};
}

} // namespace fc::patching
