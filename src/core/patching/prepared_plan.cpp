#include "prepared_plan.hpp"

#include "validation.hpp"

#include <algorithm>
#include <iterator>
#include <memory>
#include <type_traits>
#include <utility>
#include <variant>

namespace fusioncutter::patching_detail {

PreparedAllocation::PreparedAllocation(std::string name, std::shared_ptr<std::atomic<std::uintptr_t>> slot,
                                       DataAllocation memory)
    : name(std::move(name)), slot(std::move(slot)), memory(std::move(memory)) {}

PreparedAllocation::~PreparedAllocation() {
    if (slot) {
        slot->store(0, std::memory_order_release);
    }
}

} // namespace fusioncutter::patching_detail

namespace fusioncutter {

using namespace patching_detail;

PreparedPatchPlan::Impl::~Impl() {
    if (state == TransactionState::Committed || state == TransactionState::RollbackFailed) {
        retain_resources();
    } else {
        release_resources();
    }
}

void PreparedPatchPlan::Impl::retain_resources() {
    struct RetainedResources {
        std::vector<PreparedAllocation> allocations;
        std::vector<PreparedOperation> operations;
    };
    // A dropped committed or rollback-failed plan must not free native code still referencing its resources.
    static auto* retained = new RetainedResources;
    retained->allocations.insert(retained->allocations.end(), std::make_move_iterator(allocations.begin()),
                                 std::make_move_iterator(allocations.end()));
    retained->operations.insert(retained->operations.end(), std::make_move_iterator(operations.begin()),
                                std::make_move_iterator(operations.end()));
    allocations.clear();
    operations.clear();
}

std::expected<std::uintptr_t, std::string> PreparedPatchPlan::Impl::resolve(const AddressSpec& address) const {
    switch (address.kind) {
    case AddressSpec::Kind::Absolute:
        if (address.value == 0) {
            return std::unexpected("absolute destination is null");
        }
        return address.value;
    case AddressSpec::Kind::ImageRva:
        return target_address(image, static_cast<std::uint32_t>(address.value), 1);
    case AddressSpec::Kind::Allocation:
        for (const auto& allocation : allocations) {
            if (allocation.slot == address.allocation) {
                if (address.value >= allocation.memory.size()) {
                    return std::unexpected("symbolic data offset is outside its allocation");
                }
                return allocation.memory.address() + address.value;
            }
        }
        return std::unexpected("symbolic data reference belongs to another patch plan");
    case AddressSpec::Kind::Invalid:
        return std::unexpected("destination address is invalid");
    }
    return std::unexpected("destination address kind is invalid");
}

std::vector<Claim> PreparedPatchPlan::Impl::claims() const {
    std::vector<Claim> result;
    for (std::size_t index = 0; index < operations.size(); ++index) {
        const auto& operation = operations[index];
        std::visit(
            [&](const auto& operation_state) {
                using State = std::remove_cvref_t<decltype(operation_state)>;
                const auto add = [&](std::size_t size, bool mutation) {
                    result.push_back({patch_id, operation.name, operation_state.target, operation_state.target + size,
                                      index, mutation});
                };

                if constexpr (std::same_as<State, PreparedRequirement>) {
                    add(operation_state.expected.bytes.size(), false);
                } else {
                    add(operation_state.expected.bytes.size(), false);
                    if constexpr (std::same_as<State, PreparedWrite>) {
                        add(operation_state.replacement.size(), true);
                    } else if constexpr (std::same_as<State, PreparedInlineHook> ||
                                         std::same_as<State, PreparedMidHook>) {
                        add(operation_state.original.size(), true);
                    } else if constexpr (std::same_as<State, PreparedRedirect>) {
                        add(operation_state.replacement.size(), true);
                    }
                }
            },
            operation.state);
    }
    return result;
}

void PreparedPatchPlan::Impl::release_resources() {
    for (auto& operation : operations) {
        if (auto* hook = std::get_if<PreparedInlineHook>(&operation.state)) {
            hook->original_slot->store(0, std::memory_order_release);
        } else if (auto* redirect = std::get_if<PreparedRedirect>(&operation.state);
                   redirect != nullptr && redirect->original_slot) {
            redirect->original_slot->store(0, std::memory_order_release);
        }
    }
    operations.clear();
    allocations.clear();
}

std::expected<void, OutcomeReason> PreparedPatchPlan::Impl::revalidate() {
    for (auto& operation : operations) {
        auto result = std::visit(
            [&](auto& operation_state) -> std::expected<void, OutcomeReason> {
                using State = std::remove_cvref_t<decltype(operation_state)>;
                if (!pattern_matches(operation_state.target, operation_state.expected)) {
                    return std::unexpected(operation_failure("target preimage changed before commit", operation.name));
                }

                if constexpr (std::same_as<State, PreparedWrite> || std::same_as<State, PreparedRedirect>) {
                    operation_state.original = copy_bytes(operation_state.target, operation_state.replacement.size());
                } else if constexpr (std::same_as<State, PreparedInlineHook> || std::same_as<State, PreparedMidHook>) {
                    if (!equal_memory(operation_state.target, operation_state.original)) {
                        return std::unexpected(operation_failure(
                            "hook instructions changed after trampoline preparation", operation.name));
                    }
                }
                return {};
            },
            operation.state);
        if (!result) {
            return result;
        }
    }
    return {};
}

std::expected<void, OutcomeReason> PreparedPatchPlan::Impl::rollback_operations() {
    for (auto iterator = operations.rbegin(); iterator != operations.rend(); ++iterator) {
        auto& operation = *iterator;
        auto result = std::visit(
            [&](auto& operation_state) -> std::expected<void, OutcomeReason> {
                using State = std::remove_cvref_t<decltype(operation_state)>;
                if constexpr (std::same_as<State, PreparedRequirement>) {
                    return {};
                } else {
                    if (!operation_state.committed) {
                        return {};
                    }
                    if constexpr (std::same_as<State, PreparedInlineHook>) {
                        if (auto disabled = operation_state.hook.disable(); !disabled) {
                            return std::unexpected(
                                operation_failure(inline_hook_error(disabled.error()), operation.name));
                        }
                        if (!equal_memory(operation_state.target, operation_state.original)) {
                            return std::unexpected(
                                operation_failure("inline-hook rollback verification failed", operation.name));
                        }
                        operation_state.original_slot->store(0, std::memory_order_release);
                    } else if constexpr (std::same_as<State, PreparedMidHook>) {
                        if (auto disabled = operation_state.hook.disable(); !disabled) {
                            return std::unexpected(operation_failure(mid_hook_error(disabled.error()), operation.name));
                        }
                        if (!equal_memory(operation_state.target, operation_state.original)) {
                            return std::unexpected(
                                operation_failure("mid-hook rollback verification failed", operation.name));
                        }
                    } else {
                        if (!equal_memory(operation_state.target, operation_state.replacement)) {
                            return std::unexpected(
                                operation_failure("committed bytes changed before rollback", operation.name));
                        }
                        if (auto restored = write_memory(operation_state.target, operation_state.original); !restored) {
                            return std::unexpected(operation_failure(restored.error().message, operation.name));
                        }
                        if constexpr (std::same_as<State, PreparedRedirect>) {
                            if (operation_state.original_slot) {
                                operation_state.original_slot->store(0, std::memory_order_release);
                            }
                        }
                    }
                    operation_state.committed = false;
                    return {};
                }
            },
            operation.state);
        if (!result) {
            state = TransactionState::RollbackFailed;
            return result;
        }
    }
    state = TransactionState::RolledBack;
    return {};
}

PreparedPatchPlan::PreparedPatchPlan(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
PreparedPatchPlan::PreparedPatchPlan(PreparedPatchPlan&&) noexcept = default;
PreparedPatchPlan& PreparedPatchPlan::operator=(PreparedPatchPlan&&) noexcept = default;
PreparedPatchPlan::~PreparedPatchPlan() = default;

PatchId PreparedPatchPlan::patch_id() const noexcept {
    return impl_ ? impl_->patch_id : PatchId{};
}

} // namespace fusioncutter
