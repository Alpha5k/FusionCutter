#include "prepared_plan.hpp"

#include "validation.hpp"

#include <safetyhook/allocator.hpp>
#include <safetyhook/context.hpp>
#include <safetyhook/inline_hook.hpp>
#include <safetyhook/mid_hook.hpp>

#include <Windows.h>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace fusioncutter {
using namespace patching_detail;

namespace {

[[nodiscard]] bool native_architecture(Architecture architecture) noexcept {
    return architecture == (sizeof(void*) == 4 ? Architecture::X86 : Architecture::X64);
}

[[nodiscard]] std::expected<void, OutcomeReason> validate_storage(const PlanStorage& storage) {
    if (storage.patch_id.empty()) {
        return std::unexpected(operation_failure("patch ID is empty"));
    }
    if (!native_architecture(storage.image.architecture)) {
        return std::unexpected(operation_failure("target image architecture does not match this core"));
    }
    if (storage.image.base == 0 || storage.image.size == 0) {
        return std::unexpected(operation_failure("target image is empty"));
    }
    if (!storage.issues.empty()) {
        const auto& issue = storage.issues.front();
        return std::unexpected(operation_failure(issue.message, issue.operation.value_or("")));
    }

    const auto validate_address = [&](const AddressSpec& address,
                                      bool allow_allocation) -> std::expected<void, std::string> {
        switch (address.kind) {
        case AddressSpec::Kind::Absolute:
            if (address.value == 0) {
                return std::unexpected("absolute destination is null");
            }
            return {};
        case AddressSpec::Kind::ImageRva:
            if (address.value > std::numeric_limits<std::uint32_t>::max()) {
                return std::unexpected("destination RVA is invalid");
            }
            if (auto resolved = target_address(storage.image, static_cast<std::uint32_t>(address.value), 1);
                !resolved) {
                return std::unexpected(resolved.error());
            }
            return {};
        case AddressSpec::Kind::Allocation:
            if (!allow_allocation) {
                return std::unexpected("redirect destination cannot be read/write patch data");
            }
            for (const auto& candidate : storage.operations) {
                const auto* allocation = std::get_if<AllocationSpec>(&candidate.spec);
                if (allocation == nullptr || allocation->slot != address.allocation || allocation->count == 0 ||
                    allocation->element_size == 0 ||
                    allocation->count > std::numeric_limits<std::size_t>::max() / allocation->element_size) {
                    continue;
                }
                if (address.value >= allocation->count * allocation->element_size) {
                    return std::unexpected("symbolic data offset is outside its allocation");
                }
                return {};
            }
            return std::unexpected("symbolic data reference belongs to another patch plan");
        case AddressSpec::Kind::Invalid:
            return std::unexpected("destination address is invalid");
        }
        return std::unexpected("destination address kind is invalid");
    };

    for (const auto& operation : storage.operations) {
        auto valid = std::visit(
            [&](const auto& spec) -> std::expected<void, OutcomeReason> {
                using Spec = std::remove_cvref_t<decltype(spec)>;
                if constexpr (std::same_as<Spec, AllocationSpec>) {
                    if (spec.count == 0 || spec.element_size == 0 || spec.alignment == 0 ||
                        !std::has_single_bit(spec.alignment) ||
                        spec.count > std::numeric_limits<std::size_t>::max() / spec.element_size) {
                        return std::unexpected(
                            operation_failure("invalid data-allocation size or alignment", operation.name));
                    }
                    const auto size = spec.count * spec.element_size;
                    if (spec.initial_values.size() > size) {
                        return std::unexpected(
                            operation_failure("data-allocation initial values exceed its size", operation.name));
                    }
                    if (spec.proximity && !storage.image.contains_rva(spec.proximity->anchor_rva, 1)) {
                        return std::unexpected(
                            operation_failure("data-allocation near RVA is outside the target image", operation.name));
                    }
                    return {};
                } else {
                    if (!valid_pattern(spec.expected)) {
                        return std::unexpected(
                            operation_failure("invalid or unconstrained byte preimage", operation.name));
                    }
                    if (auto address = target_address(storage.image, spec.rva, spec.expected.bytes.size()); !address) {
                        return std::unexpected(operation_failure(address.error(), operation.name));
                    }
                    if (!pattern_matches(storage.image.base + spec.rva, spec.expected)) {
                        return std::unexpected(operation_failure("target preimage does not match", operation.name));
                    }

                    if constexpr (std::same_as<Spec, CheckedWriteSpec>) {
                        if (const auto* bytes = std::get_if<std::vector<std::byte>>(&spec.replacement);
                            bytes != nullptr && bytes->size() != spec.expected.bytes.size()) {
                            return std::unexpected(operation_failure(
                                "checked-write preimage and replacement sizes differ", operation.name));
                        }
                        if (const auto* replacement = std::get_if<AddressSpec>(&spec.replacement)) {
                            if (spec.expected.bytes.size() != sizeof(std::uintptr_t)) {
                                return std::unexpected(operation_failure(
                                    "pointer write preimage does not match the native pointer size", operation.name));
                            }
                            if (auto valid_address = validate_address(*replacement, true); !valid_address) {
                                return std::unexpected(operation_failure(valid_address.error(), operation.name));
                            }
                        }
                    } else if constexpr (std::same_as<Spec, InlineHookSpec>) {
                        if (spec.destination == 0) {
                            return std::unexpected(
                                operation_failure("inline-hook destination is null", operation.name));
                        }
                    } else if constexpr (std::same_as<Spec, MidHookSpec>) {
                        if (spec.callback == nullptr) {
                            return std::unexpected(operation_failure("mid-hook callback is null", operation.name));
                        }
                    } else if constexpr (std::same_as<Spec, RedirectSpec>) {
                        if (spec.expected.bytes.size() < 5) {
                            return std::unexpected(
                                operation_failure("redirect requires at least five replaceable bytes", operation.name));
                        }
                        if (spec.original_slot) {
                            const auto expected_opcode =
                                spec.kind == RedirectKind::Call ? std::byte{0xE8} : std::byte{0xE9};
                            const auto exact_direct_branch =
                                spec.expected.bytes.front() == expected_opcode &&
                                (spec.expected.mask.empty() ||
                                 std::ranges::all_of(std::span{spec.expected.mask}.first<5>(), [](std::byte mask) {
                                     return mask == std::byte{0xFF};
                                 }));
                            if (!exact_direct_branch) {
                                return std::unexpected(operation_failure(
                                    "typed original requires an exact direct call or jump preimage", operation.name));
                            }
                        }
                        if (auto valid_address = validate_address(spec.destination, false); !valid_address) {
                            return std::unexpected(operation_failure(valid_address.error(), operation.name));
                        }
                    }
                    return {};
                }
            },
            operation.spec);
        if (!valid) {
            return valid;
        }
    }
    return {};
}

[[nodiscard]] std::expected<void, OutcomeReason> allocate_data(PreparedPatchPlan::Impl& prepared,
                                                               const PlanStorage& storage) {
    for (const auto& operation : storage.operations) {
        const auto* spec = std::get_if<AllocationSpec>(&operation.spec);
        if (spec == nullptr) {
            continue;
        }

        const auto size = spec->count * spec->element_size;
        auto allocation =
            DataAllocation::create(size, spec->alignment, storage.image, spec->proximity ? &*spec->proximity : nullptr);
        if (!allocation) {
            return std::unexpected(operation_failure(allocation.error(), operation.name));
        }
        if (!spec->initial_values.empty()) {
            std::memcpy(reinterpret_cast<void*>(allocation->address()), spec->initial_values.data(),
                        spec->initial_values.size());
        }
        spec->slot->store(allocation->address(), std::memory_order_release);
        prepared.allocations.emplace_back(operation.name, spec->slot, std::move(*allocation));
    }
    return {};
}

[[nodiscard]] std::expected<PreparedWrite, OutcomeReason>
prepare_write(PreparedPatchPlan::Impl& prepared, const CheckedWriteSpec& spec, std::string_view name) {
    auto target = target_address(prepared.image, spec.rva, spec.expected.bytes.size());
    if (!target) {
        return std::unexpected(operation_failure(target.error(), name));
    }

    std::vector<std::byte> replacement;
    if (const auto* bytes = std::get_if<std::vector<std::byte>>(&spec.replacement)) {
        replacement = *bytes;
    } else {
        auto resolved = prepared.resolve(std::get<AddressSpec>(spec.replacement));
        if (!resolved) {
            return std::unexpected(operation_failure(resolved.error(), name));
        }
        const auto value = *resolved;
        const auto encoded = std::bit_cast<std::array<std::byte, sizeof(value)>>(value);
        replacement.assign(encoded.begin(), encoded.end());
    }
    return PreparedWrite{*target, spec.expected, {}, std::move(replacement)};
}

[[nodiscard]] std::expected<PreparedInlineHook, OutcomeReason>
prepare_inline(const ImageContext& image, const InlineHookSpec& spec, std::string_view name) {
    auto target = target_address(image, spec.rva, spec.expected.bytes.size());
    if (!target) {
        return std::unexpected(operation_failure(target.error(), name));
    }
    auto hook =
        safetyhook::InlineHook::create(reinterpret_cast<void*>(*target), reinterpret_cast<void*>(spec.destination),
                                       safetyhook::InlineHook::StartDisabled);
    if (!hook) {
        return std::unexpected(operation_failure(inline_hook_error(hook.error()), name));
    }
    const auto& original = hook->original_bytes();
    if (original.size() > spec.expected.bytes.size()) {
        return std::unexpected(
            operation_failure("inline-hook preimage does not cover SafetyHook's instruction span", name));
    }
    return PreparedInlineHook{*target,
                              spec.expected,
                              {reinterpret_cast<const std::byte*>(original.data()),
                               reinterpret_cast<const std::byte*>(original.data() + original.size())},
                              std::move(*hook),
                              spec.original_slot};
}

[[nodiscard]] std::expected<PreparedMidHook, OutcomeReason>
prepare_mid(const ImageContext& image, const MidHookSpec& spec, std::string_view name) {
    static_assert(sizeof(SimdRegister) == sizeof(safetyhook::Xmm));
    static_assert(alignof(SimdRegister) == alignof(safetyhook::Xmm));
    static_assert(sizeof(MidHookContext) == sizeof(safetyhook::Context));
    static_assert(alignof(MidHookContext) == alignof(safetyhook::Context));
    // The adapter passes SafetyHook's saved register block through Fusion Cutter's reviewed layout.
#if defined(_M_X64)
    static_assert(offsetof(MidHookContext, xmm0) == offsetof(safetyhook::Context, xmm0));
    static_assert(offsetof(MidHookContext, rax) == offsetof(safetyhook::Context, rax));
    static_assert(offsetof(MidHookContext, rsp) == offsetof(safetyhook::Context, rsp));
    static_assert(offsetof(MidHookContext, rip) == offsetof(safetyhook::Context, rip));
#else
    static_assert(offsetof(MidHookContext, xmm0) == offsetof(safetyhook::Context, xmm0));
    static_assert(offsetof(MidHookContext, eax) == offsetof(safetyhook::Context, eax));
    static_assert(offsetof(MidHookContext, esp) == offsetof(safetyhook::Context, esp));
    static_assert(offsetof(MidHookContext, eip) == offsetof(safetyhook::Context, eip));
#endif

    auto target = target_address(image, spec.rva, spec.expected.bytes.size());
    if (!target) {
        return std::unexpected(operation_failure(target.error(), name));
    }
    auto hook = safetyhook::MidHook::create(reinterpret_cast<void*>(*target),
                                            reinterpret_cast<safetyhook::MidHookFn>(spec.callback),
                                            safetyhook::MidHook::StartDisabled);
    if (!hook) {
        return std::unexpected(operation_failure(mid_hook_error(hook.error()), name));
    }
    const auto& original = hook->original_bytes();
    if (original.size() > spec.expected.bytes.size()) {
        return std::unexpected(
            operation_failure("mid-hook preimage does not cover SafetyHook's instruction span", name));
    }
    return PreparedMidHook{*target,
                           spec.expected,
                           {reinterpret_cast<const std::byte*>(original.data()),
                            reinterpret_cast<const std::byte*>(original.data() + original.size())},
                           std::move(*hook)};
}

[[nodiscard]] bool rel32_reachable(std::uintptr_t source_after, std::uintptr_t destination) noexcept {
    const auto difference = static_cast<std::int64_t>(destination) - static_cast<std::int64_t>(source_after);
    return difference >= std::numeric_limits<std::int32_t>::min() &&
           difference <= std::numeric_limits<std::int32_t>::max();
}

void encode_rel32(std::span<std::byte> output, RedirectKind kind, std::uintptr_t source, std::uintptr_t destination) {
    output.front() = kind == RedirectKind::Call ? std::byte{0xE8} : std::byte{0xE9};
    const auto difference = static_cast<std::int64_t>(destination) - static_cast<std::int64_t>(source + 5);
    const auto displacement = static_cast<std::int32_t>(difference);
    std::memcpy(output.data() + 1, &displacement, sizeof(displacement));
}

[[nodiscard]] std::expected<PreparedRedirect, OutcomeReason>
prepare_redirect(PreparedPatchPlan::Impl& prepared, const RedirectSpec& spec, std::string_view name) {
    auto target = target_address(prepared.image, spec.rva, spec.expected.bytes.size());
    if (!target) {
        return std::unexpected(operation_failure(target.error(), name));
    }
    auto destination = prepared.resolve(spec.destination);
    if (!destination) {
        return std::unexpected(operation_failure(destination.error(), name));
    }

    safetyhook::Allocation relay;
    auto branch_destination = *destination;
    if (!rel32_reachable(*target + 5, branch_destination)) {
        if constexpr (sizeof(void*) == 4) {
            return std::unexpected(operation_failure("redirect destination is outside the x86 rel32 range", name));
        } else {
            constexpr std::size_t kRelaySize = 14;
            auto allocation =
                safetyhook::Allocator::global()->allocate_near({reinterpret_cast<std::uint8_t*>(*target)}, kRelaySize);
            if (!allocation) {
                return std::unexpected(operation_failure("could not allocate an x64 redirect relay", name));
            }
            relay = std::move(*allocation);
            std::array<std::byte, kRelaySize> bytes{
                std::byte{0xFF}, std::byte{0x25}, std::byte{}, std::byte{}, std::byte{}, std::byte{}, std::byte{},
                std::byte{},     std::byte{},     std::byte{}, std::byte{}, std::byte{}, std::byte{}, std::byte{}};
            std::memcpy(bytes.data() + 6, &*destination, sizeof(*destination));
            std::memcpy(relay.data(), bytes.data(), bytes.size());
            if (!FlushInstructionCache(GetCurrentProcess(), relay.data(), relay.size())) {
                return std::unexpected(operation_failure("could not flush the x64 redirect relay", name));
            }
            branch_destination = relay.address();
        }
    }

    std::vector<std::byte> replacement(spec.expected.bytes.size(), std::byte{0x90});
    encode_rel32(replacement, spec.kind, *target, branch_destination);

    std::uintptr_t original_destination{};
    if (spec.original_slot) {
        std::int32_t displacement{};
        std::memcpy(&displacement, reinterpret_cast<const void*>(*target + 1), sizeof(displacement));
        const auto next_instruction = *target + 5;
        if (displacement >= 0) {
            const auto distance = static_cast<std::uintptr_t>(displacement);
            if (next_instruction > std::numeric_limits<std::uintptr_t>::max() - distance) {
                return std::unexpected(operation_failure("direct branch target overflows the address space", name));
            }
            original_destination = next_instruction + distance;
        } else {
            const auto distance = static_cast<std::uintptr_t>(-static_cast<std::int64_t>(displacement));
            if (next_instruction < distance) {
                return std::unexpected(operation_failure("direct branch target underflows the address space", name));
            }
            original_destination = next_instruction - distance;
        }
        if (original_destination == 0) {
            return std::unexpected(operation_failure("direct branch target is null", name));
        }
    }

    return PreparedRedirect{
        *target, spec.expected, {}, std::move(replacement), std::move(relay), original_destination, spec.original_slot};
}

[[nodiscard]] std::expected<void, OutcomeReason> prepare_operations(PreparedPatchPlan::Impl& prepared,
                                                                    const PlanStorage& storage) {
    for (const auto& operation : storage.operations) {
        if (std::holds_alternative<AllocationSpec>(operation.spec)) {
            continue;
        }

        auto state = std::visit(
            [&](const auto& spec) -> std::expected<PreparedOperationState, OutcomeReason> {
                using Spec = std::remove_cvref_t<decltype(spec)>;
                if constexpr (std::same_as<Spec, CheckedWriteSpec>) {
                    auto result = prepare_write(prepared, spec, operation.name);
                    if (!result) {
                        return std::unexpected(result.error());
                    }
                    return PreparedOperationState{std::move(*result)};
                } else if constexpr (std::same_as<Spec, InlineHookSpec>) {
                    auto result = prepare_inline(prepared.image, spec, operation.name);
                    if (!result) {
                        return std::unexpected(result.error());
                    }
                    return PreparedOperationState{std::move(*result)};
                } else if constexpr (std::same_as<Spec, MidHookSpec>) {
                    auto result = prepare_mid(prepared.image, spec, operation.name);
                    if (!result) {
                        return std::unexpected(result.error());
                    }
                    return PreparedOperationState{std::move(*result)};
                } else if constexpr (std::same_as<Spec, RedirectSpec>) {
                    auto result = prepare_redirect(prepared, spec, operation.name);
                    if (!result) {
                        return std::unexpected(result.error());
                    }
                    return PreparedOperationState{std::move(*result)};
                } else if constexpr (std::same_as<Spec, RequirementSpec>) {
                    auto target = target_address(prepared.image, spec.rva, spec.expected.bytes.size());
                    if (!target) {
                        return std::unexpected(operation_failure(target.error(), operation.name));
                    }
                    return PreparedOperationState{PreparedRequirement{*target, spec.expected}};
                } else {
                    return std::unexpected(operation_failure("unexpected allocation operation", operation.name));
                }
            },
            operation.spec);
        if (!state) {
            return std::unexpected(state.error());
        }
        prepared.operations.push_back({operation.name, std::move(*state)});
    }
    return {};
}

[[nodiscard]] std::expected<void, OutcomeReason> validate_preimages(const PreparedPatchPlan::Impl& prepared) {
    for (const auto& operation : prepared.operations) {
        const auto matches = std::visit(
            [](const auto& state) {
                return pattern_matches(state.target, state.expected);
            },
            operation.state);
        if (!matches) {
            return std::unexpected(operation_failure("target preimage does not match", operation.name));
        }
    }
    return {};
}

} // namespace

std::expected<PreparedPatchPlan, OutcomeReason> PreparedPatchPlan::prepare(PatchPlan&& plan) {
    if (!plan.storage_) {
        return std::unexpected(operation_failure("patch plan has already been consumed"));
    }
    auto storage = std::move(plan.storage_);
    if (auto valid = validate_storage(*storage); !valid) {
        return std::unexpected(valid.error());
    }

    auto prepared = std::make_unique<Impl>();
    prepared->patch_id = storage->patch_id;
    prepared->image = storage->image;
    if (auto allocated = allocate_data(*prepared, *storage); !allocated) {
        return std::unexpected(allocated.error());
    }
    if (auto operations = prepare_operations(*prepared, *storage); !operations) {
        return std::unexpected(operations.error());
    }
    if (auto preimages = validate_preimages(*prepared); !preimages) {
        return std::unexpected(preimages.error());
    }
    return PreparedPatchPlan{std::move(prepared)};
}

} // namespace fusioncutter
