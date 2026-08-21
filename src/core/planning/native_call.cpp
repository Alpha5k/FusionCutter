#include "native_call.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <ranges>
#include <vector>

namespace fc::planning {
namespace {

// Normalizes register and stack homes into comparable extents for overlap validation.
struct StorageExtent {
    FC_NativeStorageKind kind{};
    FC_NativeRegister register_id{};
    std::uint32_t begin{};
    std::uint32_t end{};
};

// These predicates validate logical values and their architecture-specific physical storage before normalization.
[[nodiscard]] bool power_of_two(std::uint32_t value) noexcept {
    return value != 0 && (value & (value - 1)) == 0;
}

[[nodiscard]] std::uint32_t pointer_size(FC_Architecture architecture) noexcept {
    return architecture == FC_ARCH_X86 ? 4U : 8U;
}

[[nodiscard]] bool valid_value(const FC_NativeValue& value, FC_Architecture architecture, bool allow_void) noexcept {
    switch (value.kind) {
    case FC_NATIVE_VOID:
        return allow_void && value.size == 0 && value.alignment == 0;
    case FC_NATIVE_INTEGER:
        return (value.size == 1 || value.size == 2 || value.size == 4 || value.size == 8) &&
               power_of_two(value.alignment) && value.alignment <= value.size;
    case FC_NATIVE_POINTER:
        return value.size == pointer_size(architecture) && value.alignment == value.size;
    case FC_NATIVE_FLOAT_32:
        return value.size == 4 && value.alignment == 4;
    case FC_NATIVE_FLOAT_64:
        return value.size == 8 && value.alignment == 8;
    case FC_NATIVE_RECORD:
        return value.size != 0 && power_of_two(value.alignment) && value.alignment <= value.size &&
               value.size % value.alignment == 0;
    default:
        return false;
    }
}

[[nodiscard]] bool general_register(FC_NativeRegister id, FC_Architecture architecture) noexcept {
    if (architecture == FC_ARCH_X86) {
        return id >= FC_REGISTER_EAX && id <= FC_REGISTER_EBP;
    }
    return id >= FC_REGISTER_RAX && id <= FC_REGISTER_R15;
}

[[nodiscard]] bool simd_register(FC_NativeRegister id, FC_Architecture architecture) noexcept {
    if (architecture == FC_ARCH_X86) {
        return id >= FC_REGISTER_XMM0 && id <= FC_REGISTER_XMM7;
    }
    return id >= FC_REGISTER_XMM0 && id <= FC_REGISTER_XMM15;
}

[[nodiscard]] bool register_holds(const FC_NativeValue& value, FC_NativeRegister id, FC_Architecture architecture,
                                  bool result) noexcept {
    if (value.kind == FC_NATIVE_FLOAT_32 || value.kind == FC_NATIVE_FLOAT_64) {
        if (result && architecture == FC_ARCH_X86) {
            return id == FC_REGISTER_ST0;
        }
        return simd_register(id, architecture);
    }
    const auto capacity = pointer_size(architecture);
    return general_register(id, architecture) && value.size <= capacity;
}

[[nodiscard]] std::optional<StorageExtent> validate_storage(const FC_NativeStorage& storage,
                                                            const FC_NativeValue& value, FC_Architecture architecture,
                                                            std::uint32_t stack_size, bool result,
                                                            bool allow_none) noexcept {
    // Normalize every accepted home so argument overlap checks do not need architecture-specific cases.
    if (storage.kind == FC_NATIVE_STORAGE_NONE) {
        if (!allow_none || storage.register_id != FC_REGISTER_NONE || storage.stack_offset != 0) {
            return std::nullopt;
        }
        return StorageExtent{.kind = storage.kind};
    }
    if (storage.kind == FC_NATIVE_STORAGE_REGISTER) {
        if (storage.stack_offset != 0 || storage.register_id == FC_REGISTER_NONE ||
            !register_holds(value, storage.register_id, architecture, result)) {
            return std::nullopt;
        }
        return StorageExtent{.kind = storage.kind, .register_id = storage.register_id};
    }
    if (storage.kind != FC_NATIVE_STORAGE_STACK || storage.register_id != FC_REGISTER_NONE || value.alignment == 0 ||
        storage.stack_offset % value.alignment != 0 || storage.stack_offset > stack_size ||
        value.size > stack_size - storage.stack_offset) {
        return std::nullopt;
    }
    return StorageExtent{.kind = storage.kind,
                         .begin = storage.stack_offset,
                         .end = static_cast<std::uint32_t>(storage.stack_offset + value.size)};
}

[[nodiscard]] bool overlaps(const StorageExtent& left, const StorageExtent& right) noexcept {
    if (left.kind != right.kind || left.kind == FC_NATIVE_STORAGE_NONE) {
        return false;
    }
    if (left.kind == FC_NATIVE_STORAGE_REGISTER) {
        return left.register_id == right.register_id;
    }
    return left.begin < right.end && right.begin < left.end;
}

[[nodiscard]] bool equal_value(const FC_NativeValue& left, const FC_NativeValue& right) noexcept {
    return left.kind == right.kind && left.size == right.size && left.alignment == right.alignment;
}

[[nodiscard]] bool equal_storage(const FC_NativeStorage& left, const FC_NativeStorage& right) noexcept {
    return left.kind == right.kind && left.register_id == right.register_id && left.stack_offset == right.stack_offset;
}

} // namespace

std::expected<NativeCallRecord, std::string> validate_native_call(const FC_NativeCall& call,
                                                                  FC_Architecture architecture) {
    // Validate the top-level ABI record before following its borrowed argument pointer.
    constexpr auto required_size = offsetof(FC_NativeCall, stack_size) + sizeof(call.stack_size);
    if (call.struct_size < required_size || (architecture != FC_ARCH_X86 && architecture != FC_ARCH_X64) ||
        !valid_value(call.result, architecture, true) || (call.argument_count != 0 && call.arguments == nullptr) ||
        call.argument_count > kPatchPlanByteCapacity / sizeof(FC_NativeArgument)) {
        return std::unexpected("Native call has an invalid record prefix, value, or argument view");
    }
    // Cleanup describes the complete normalized stack area and therefore cannot be inferred per argument.
    if ((architecture == FC_ARCH_X64 && call.cleanup != FC_STACK_CLEANUP_NONE) ||
        (architecture == FC_ARCH_X86 && ((call.stack_size == 0 && call.cleanup != FC_STACK_CLEANUP_NONE) ||
                                         (call.stack_size != 0 && call.cleanup != FC_STACK_CLEANUP_CALLER &&
                                          call.cleanup != FC_STACK_CLEANUP_CALLEE)))) {
        return std::unexpected("Native call cleanup does not match the selected architecture and stack extent");
    }

    NativeCallRecord result{.result = call.result,
                            .return_storage = call.return_storage,
                            .cleanup = call.cleanup,
                            .stack_size = call.stack_size};
    result.arguments.reserve(call.argument_count);
    std::vector<StorageExtent> homes;
    homes.reserve(call.argument_count + 1);
    // Argument order is logical signature order; homes are checked pairwise as they enter the owned record.
    for (std::uint32_t index = 0; index < call.argument_count; ++index) {
        const auto& argument = call.arguments[index];
        if (!valid_value(argument.value, architecture, false)) {
            return std::unexpected("Native call argument has an invalid logical value");
        }
        const auto home =
            validate_storage(argument.storage, argument.value, architecture, call.stack_size, false, false);
        if (!home || std::ranges::any_of(homes, [&](const StorageExtent& prior) {
                return overlaps(*home, prior);
            })) {
            return std::unexpected("Native call argument has an invalid or overlapping physical home");
        }
        homes.push_back(*home);
        result.arguments.push_back(argument);
    }

    // Direct results may reuse consumed argument registers; pointers to hidden results may not overlap any input home.
    if (call.result.kind == FC_NATIVE_VOID) {
        if (!validate_storage(call.return_storage, call.result, architecture, call.stack_size, true, true)) {
            return std::unexpected("Void native call result must use empty storage");
        }
    } else {
        const auto physical_value =
            call.result.kind == FC_NATIVE_RECORD
                ? FC_NativeValue{FC_NATIVE_POINTER, pointer_size(architecture), pointer_size(architecture)}
                : call.result;
        const auto home =
            validate_storage(call.return_storage, physical_value, architecture, call.stack_size, true, false);
        if (!home ||
            (call.result.kind == FC_NATIVE_RECORD && std::ranges::any_of(homes, [&](const StorageExtent& argument) {
                 return overlaps(*home, argument);
             }))) {
            return std::unexpected("Native call result has an invalid physical home");
        }
    }
    return result;
}

bool equivalent_native_call(const NativeCallRecord& left, const NativeCallRecord& right) noexcept {
    if (!equal_value(left.result, right.result) || !equal_storage(left.return_storage, right.return_storage) ||
        left.cleanup != right.cleanup || left.stack_size != right.stack_size ||
        left.arguments.size() != right.arguments.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.arguments.size(); ++index) {
        if (!equal_value(left.arguments[index].value, right.arguments[index].value) ||
            !equal_storage(left.arguments[index].storage, right.arguments[index].storage)) {
            return false;
        }
    }
    return true;
}

} // namespace fc::planning
