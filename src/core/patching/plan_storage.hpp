#pragma once

#include "FusionCutter/patching.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace fusioncutter::patching_detail {

struct OwnedPattern {
    std::vector<std::byte> bytes;
    std::vector<std::byte> mask;
};

struct AddressSpec {
    enum class Kind {
        Invalid,
        Absolute,
        ImageRva,
        Allocation,
    };

    Kind kind{Kind::Invalid};
    std::uintptr_t value{};
    std::shared_ptr<std::atomic<std::uintptr_t>> allocation;
};

struct CheckedWriteSpec {
    std::uint32_t rva;
    OwnedPattern expected;
    std::variant<std::vector<std::byte>, AddressSpec> replacement;
};

struct InlineHookSpec {
    std::uint32_t rva;
    OwnedPattern expected;
    std::uintptr_t destination;
    std::shared_ptr<std::atomic<std::uintptr_t>> original_slot;
};

struct MidHookSpec {
    std::uint32_t rva;
    OwnedPattern expected;
    MidHookCallback callback;
};

struct RedirectSpec {
    std::uint32_t rva;
    OwnedPattern expected;
    RedirectKind kind;
    AddressSpec destination;
    std::shared_ptr<std::atomic<std::uintptr_t>> original_slot;
};

struct AllocationSpec {
    std::size_t count;
    std::size_t element_size;
    std::size_t alignment;
    std::vector<std::byte> initial_values;
    std::optional<NearConstraint> proximity;
    std::shared_ptr<std::atomic<std::uintptr_t>> slot;
};

struct RequirementSpec {
    std::uint32_t rva;
    OwnedPattern expected;
};

using OperationSpec =
    std::variant<CheckedWriteSpec, InlineHookSpec, MidHookSpec, RedirectSpec, AllocationSpec, RequirementSpec>;

struct PlanOperation {
    std::string name;
    OperationSpec spec;
};

struct PlanIssue {
    std::string message;
    std::optional<std::string> operation;
};

class PlanStorage {
  public:
    PlanStorage(PatchId patch_id, ImageContext image) : patch_id(patch_id), image(image) {}

    [[nodiscard]] static AddressSpec copy_address(const PatchAddress& address) {
        AddressSpec result;
        switch (address.kind_) {
        case PatchAddress::Kind::Absolute:
            result.kind = AddressSpec::Kind::Absolute;
            break;
        case PatchAddress::Kind::ImageRva:
            result.kind = AddressSpec::Kind::ImageRva;
            break;
        case PatchAddress::Kind::Allocation:
            result.kind = AddressSpec::Kind::Allocation;
            break;
        case PatchAddress::Kind::Invalid:
            break;
        }
        result.value = address.value_;
        result.allocation = address.allocation_;
        return result;
    }

    PatchId patch_id;
    ImageContext image;
    std::vector<PlanOperation> operations;
    std::vector<PlanIssue> issues;
};

} // namespace fusioncutter::patching_detail
