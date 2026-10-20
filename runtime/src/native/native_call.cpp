#include "native_call.hpp"

#include "bridge.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>

namespace fc::runtime {

namespace {

// Validates logical size and alignment for direct calls and bridges, with optional void results.
// Pointer size follows architecture; call-layout validation checks register and stack storage separately.
bool valid_value(const FC_NativeValue &value, FC_Architecture architecture, bool allow_void) noexcept {
    switch (value.kind) {
    case FC_NATIVE_VOID:
        return allow_void && value.size == 0 && value.alignment == 0;
    case FC_NATIVE_INTEGER:
        return (value.size == 1 || value.size == 2 || value.size == 4 || value.size == 8) && std::has_single_bit(value.alignment) &&
               value.alignment <= value.size;
    case FC_NATIVE_POINTER:
        return value.size == (architecture == FC_ARCH_X86 ? 4U : 8U) && value.alignment == value.size;
    case FC_NATIVE_FLOAT_32:
        return value.size == 4 && value.alignment == 4;
    case FC_NATIVE_FLOAT_64:
        return value.size == 8 && value.alignment == 8;
    case FC_NATIVE_RECORD:
        // Records occupy complete alignment units so arrays and copied values preserve their declared layout.
        return value.size != 0 && std::has_single_bit(value.alignment) && value.alignment <= value.size &&
               value.size % value.alignment == 0;
    default:
        return false;
    }
}

// Compare the logical kind and byte layout required for the same argument or result.
bool equal_value(const FC_NativeValue &left, const FC_NativeValue &right) noexcept {
    return left.kind == right.kind && left.size == right.size && left.alignment == right.alignment;
}

// Compare physical register or normalized stack locations, including unused fields.
bool equal_storage(const FC_NativeStorage &left, const FC_NativeStorage &right) noexcept {
    return left.kind == right.kind && left.register_id == right.register_id && left.stack_offset == right.stack_offset;
}

} // namespace

std::expected<CallDescription, Error> validate_call(FC_NativeCall call, bool bridge) {
#if defined(_WIN64)
    constexpr auto current_architecture = FC_ARCH_X64;
#else
    constexpr auto current_architecture = FC_ARCH_X86;
#endif

    // Logical values and pointer widths must match the executing architecture before physical storage is checked.
    const auto architecture = static_cast<FC_Architecture>(call.architecture);
    if ((architecture != current_architecture) || !valid_value(call.result, architecture, true) ||
        (call.argument_count && !call.arguments) || (call.flags & ~(FC_CALL_COMPILER | FC_CALL_VARIADIC | FC_CALL_HIDDEN_RESULT))) {
        return std::unexpected(Error{"Native call has an invalid architecture, result description, argument view or flags"});
    }

    for (std::uint32_t index = 0; index < call.argument_count; ++index) {
        if (!valid_value(call.arguments[index].value, architecture, false)) {
            return std::unexpected(Error{"Native call argument has an invalid kind, size or alignment"});
        }
    }

    // The bridge owns physical capability checks; compiler direct calls retain their native C++ signature.
    if (bridge) {
        auto supported = native::validate_bridge_layout(call);
        if (!supported) {
            return std::unexpected(Error{supported.error()});
        }
    }

    // Own the argument descriptions so subsequent emission is independent of the caller's declaration storage.
    CallDescription result;
    result.value = call;
    result.value.arguments = nullptr;
    if (call.argument_count) {
        result.arguments.assign(call.arguments, call.arguments + call.argument_count);
    }
    return result;
}

FC_NativeCall CallDescription::view() const noexcept {
    // Rebind the argument view after copies or moves of the owning description.
    auto result = value;
    result.arguments = arguments.empty() ? nullptr : arguments.data();
    result.argument_count = static_cast<std::uint32_t>(arguments.size());
    return result;
}

bool equivalent_call(const CallDescription &a, const CallDescription &b) noexcept {
    const auto left = a.view(), right = b.view();
    if (left.architecture != right.architecture || left.flags != right.flags) {
        return false;
    }
    // Shared hook sites require identical logical values, physical storage, and stack cleanup conventions.
    if (!equal_value(left.result, right.result) || !equal_storage(left.return_storage, right.return_storage) ||
        left.cleanup != right.cleanup || left.stack_size != right.stack_size || left.argument_count != right.argument_count) {
        return false;
    }

    for (std::size_t index = 0; index < left.argument_count; ++index) {
        if (!equal_value(left.arguments[index].value, right.arguments[index].value) ||
            !equal_storage(left.arguments[index].storage, right.arguments[index].storage)) {
            return false;
        }
    }

    return true;
}

} // namespace fc::runtime
