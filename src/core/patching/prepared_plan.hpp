#pragma once

#include "memory.hpp"
#include "patching.hpp"
#include "plan_storage.hpp"

#include <safetyhook/allocator.hpp>
#include <safetyhook/inline_hook.hpp>
#include <safetyhook/mid_hook.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace fusioncutter::patching_detail {

struct PreparedWrite {
    std::uintptr_t target;
    OwnedPattern expected;
    std::vector<std::byte> original;
    std::vector<std::byte> replacement;
    bool committed{};
};

struct PreparedInlineHook {
    std::uintptr_t target;
    OwnedPattern expected;
    std::vector<std::byte> original;
    safetyhook::InlineHook hook;
    std::shared_ptr<std::atomic<std::uintptr_t>> original_slot;
    bool committed{};
};

struct PreparedMidHook {
    std::uintptr_t target;
    OwnedPattern expected;
    std::vector<std::byte> original;
    safetyhook::MidHook hook;
    bool committed{};
};

struct PreparedRedirect {
    std::uintptr_t target;
    OwnedPattern expected;
    std::vector<std::byte> original;
    std::vector<std::byte> replacement;
    safetyhook::Allocation relay;
    std::uintptr_t original_destination{};
    std::shared_ptr<std::atomic<std::uintptr_t>> original_slot;
    bool committed{};
};

struct PreparedRequirement {
    std::uintptr_t target;
    OwnedPattern expected;
};

using PreparedOperationState =
    std::variant<PreparedWrite, PreparedInlineHook, PreparedMidHook, PreparedRedirect, PreparedRequirement>;

struct PreparedOperation {
    std::string name;
    PreparedOperationState state;
};

struct PreparedAllocation {
    PreparedAllocation(std::string name, std::shared_ptr<std::atomic<std::uintptr_t>> slot, DataAllocation memory);
    PreparedAllocation(const PreparedAllocation&) = delete;
    PreparedAllocation(PreparedAllocation&&) noexcept = default;
    PreparedAllocation& operator=(const PreparedAllocation&) = delete;
    PreparedAllocation& operator=(PreparedAllocation&&) noexcept = default;
    ~PreparedAllocation();

    std::string name;
    std::shared_ptr<std::atomic<std::uintptr_t>> slot;
    DataAllocation memory;
};

struct Claim {
    PatchId patch_id;
    std::string operation;
    std::uintptr_t begin;
    std::uintptr_t end;
    std::size_t operation_index;
    bool mutation;
};

enum class TransactionState {
    Prepared,
    Committed,
    RolledBack,
    RollbackFailed,
};

} // namespace fusioncutter::patching_detail

namespace fusioncutter {

class PreparedPatchPlan::Impl {
  public:
    ~Impl();

    [[nodiscard]] std::expected<std::uintptr_t, std::string> resolve(const patching_detail::AddressSpec& address) const;
    [[nodiscard]] std::vector<patching_detail::Claim> claims() const;
    [[nodiscard]] std::expected<void, OutcomeReason> revalidate();
    [[nodiscard]] std::expected<void, OutcomeReason> rollback_operations();
    void release_resources();

    PatchId patch_id;
    ImageContext image;
    std::vector<patching_detail::PreparedAllocation> allocations;
    std::vector<patching_detail::PreparedOperation> operations;
    patching_detail::TransactionState state{patching_detail::TransactionState::Prepared};
    bool ranges_reserved{};

  private:
    void retain_resources();
};

} // namespace fusioncutter
