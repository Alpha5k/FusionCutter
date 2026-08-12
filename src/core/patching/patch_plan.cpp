#include "FusionCutter/patching.hpp"

#include "plan_storage.hpp"

#include <algorithm>
#include <limits>
#include <utility>
#include <vector>

namespace fusioncutter {
namespace {

using patching_detail::AddressSpec;
using patching_detail::OwnedPattern;
using patching_detail::PlanStorage;

[[nodiscard]] OwnedPattern copy_pattern(BytePattern pattern) {
    return {{pattern.bytes.begin(), pattern.bytes.end()}, {pattern.mask.begin(), pattern.mask.end()}};
}

void add_operation(PlanStorage& storage, std::string_view name, patching_detail::OperationSpec spec) {
    if (name.empty()) {
        storage.issues.push_back({"operation name is empty", std::nullopt});
    }
    storage.operations.push_back({std::string{name}, std::move(spec)});
}

} // namespace

PatchAddress PatchAddress::image_rva(std::uint32_t rva) noexcept {
    PatchAddress address;
    address.kind_ = Kind::ImageRva;
    address.value_ = rva;
    return address;
}

PatchAddress PatchAddress::allocation(std::shared_ptr<std::atomic<std::uintptr_t>> slot,
                                      std::size_t byte_offset) noexcept {
    PatchAddress address;
    address.kind_ = Kind::Allocation;
    address.value_ = byte_offset;
    address.allocation_ = std::move(slot);
    return address;
}

PatchPlan::PatchPlan(PatchId patch_id, ImageContext image) : storage_(std::make_unique<PlanStorage>(patch_id, image)) {}

PatchPlan::PatchPlan(PatchPlan&&) noexcept = default;
PatchPlan& PatchPlan::operator=(PatchPlan&&) noexcept = default;
PatchPlan::~PatchPlan() = default;

void PatchPlan::checked_write(std::string_view operation, std::uint32_t rva, BytePattern expected,
                              std::span<const std::byte> replacement) {
    add_operation(*storage_, operation,
                  patching_detail::CheckedWriteSpec{rva, copy_pattern(expected),
                                                    std::vector<std::byte>{replacement.begin(), replacement.end()}});
}

void PatchPlan::checked_write(std::string_view operation, std::uint32_t rva, BytePattern expected,
                              PatchAddress replacement) {
    add_operation(
        *storage_, operation,
        patching_detail::CheckedWriteSpec{rva, copy_pattern(expected), PlanStorage::copy_address(replacement)});
}

void PatchPlan::nop(std::string_view operation, std::uint32_t rva, BytePattern expected) {
    std::vector<std::byte> replacement(expected.bytes.size(), std::byte{0x90});
    checked_write(operation, rva, expected, replacement);
}

void PatchPlan::require_bytes(std::string_view operation, std::uint32_t rva, BytePattern expected) {
    add_operation(*storage_, operation, patching_detail::RequirementSpec{rva, copy_pattern(expected)});
}

void PatchPlan::add_inline_hook(std::string_view operation, std::uint32_t rva, BytePattern expected,
                                std::uintptr_t destination,
                                std::shared_ptr<std::atomic<std::uintptr_t>> original_slot) {
    add_operation(*storage_, operation,
                  patching_detail::InlineHookSpec{rva, copy_pattern(expected), destination, std::move(original_slot)});
}

void PatchPlan::mid_hook(std::string_view operation, std::uint32_t rva, BytePattern expected,
                         MidHookCallback callback) {
    add_operation(*storage_, operation, patching_detail::MidHookSpec{rva, copy_pattern(expected), callback});
}

void PatchPlan::redirect(std::string_view operation, std::uint32_t rva, BytePattern expected, RedirectKind kind,
                         PatchAddress destination) {
    add_redirect(operation, rva, expected, kind, std::move(destination));
}

void PatchPlan::add_redirect(std::string_view operation, std::uint32_t rva, BytePattern expected, RedirectKind kind,
                             PatchAddress destination, std::shared_ptr<std::atomic<std::uintptr_t>> original_slot) {
    add_operation(*storage_, operation,
                  patching_detail::RedirectSpec{rva, copy_pattern(expected), kind,
                                                PlanStorage::copy_address(destination), std::move(original_slot)});
}

void PatchPlan::add_allocation(std::string_view operation, std::size_t count, std::size_t element_size,
                               std::size_t alignment, std::span<const std::byte> initial_values,
                               std::optional<AllocationProximity> proximity,
                               std::shared_ptr<std::atomic<std::uintptr_t>> slot) {
    add_operation(*storage_, operation,
                  patching_detail::AllocationSpec{count,
                                                  element_size,
                                                  alignment,
                                                  {initial_values.begin(), initial_values.end()},
                                                  proximity,
                                                  std::move(slot)});
}

} // namespace fusioncutter
