#include "bridge_layout.hpp"

#include <algorithm>
#include <limits>
#include <span>
#include <stdexcept>

namespace fc::runtime::native {

namespace bridge_detail {

bool hidden_result(const FC_NativeCall &call) {
    return (call.flags & FC_CALL_HIDDEN_RESULT) != 0;
}

std::optional<std::uint8_t> general(std::uint32_t id) {
    // ABI IDs list registers by name; ModRM and opcode fields use the processor's register numbering.
    if constexpr (kWord == 4) {
        if (id >= FC_REGISTER_EAX && id <= FC_REGISTER_EBP) {
            constexpr std::array<std::uint8_t, 7> numbers{0, 3, 1, 2, 6, 7, 5};
            return numbers[id - FC_REGISTER_EAX];
        }
    }

    if constexpr (kWord == 8) {
        if (id >= FC_REGISTER_RAX && id <= FC_REGISTER_R15) {
            constexpr std::array<std::uint8_t, 15> numbers{0, 3, 1, 2, 6, 7, 5, 8, 9, 10, 11, 12, 13, 14, 15};
            return numbers[id - FC_REGISTER_RAX];
        }
    }
    return std::nullopt;
}

std::optional<std::uint8_t> simd(std::uint32_t id) {
    if (id >= FC_REGISTER_XMM0 && id < FC_REGISTER_XMM0 + kSimdCount) {
        return static_cast<std::uint8_t>(id - FC_REGISTER_XMM0);
    }
    return std::nullopt;
}

namespace {

// Reserve aligned stack bytes, advance extent and return the starting offset; alignment must be a nonzero power of two.
std::uint32_t reserve(std::uint32_t &extent, std::uint64_t size, std::uint32_t alignment = kWord) {
    // Widen alignment arithmetic and bound every offset for signed 32-bit instruction displacements.
    const auto aligned = (static_cast<std::uint64_t>(extent) + alignment - 1) & ~(std::uint64_t{alignment} - 1);
    const auto end = aligned + size;
    if (end > std::numeric_limits<std::int32_t>::max() - kSaved - 64) {
        throw std::length_error("Native bridge stack storage is too large for the required signed 32-bit offsets");
    }

    extent = static_cast<std::uint32_t>(end);
    return static_cast<std::uint32_t>(aligned);
}

} // namespace

FrameLayout layout(const FC_NativeCall &call, bool entry) {
    FrameLayout result;
    // Keep outgoing arguments below private storage; x64 callbacks also require 32 bytes of home space.
    std::uint32_t extent = entry ? (kWord == 4 ? 16 : kHome) : kHome + call.stack_size;
    if constexpr (kWord == 4) {
        result.anchor = reserve(extent, kWord);
    }
    if (!entry) {
        result.target = reserve(extent, kWord);
        result.flag_pointer = reserve(extent, kWord);
    }
    result.result_pointer = reserve(extent, kWord);
    result.flags = reserve(extent, kWord);
    if (call.result.kind != FC_NATIVE_VOID && !hidden_result(call)) {
        // Entry results need alignment slack; invokers need word-sized scratch even for a narrow general result.
        result.result = reserve(extent, std::max(call.result.size, kWord) + std::uint64_t{entry ? call.result.alignment - 1 : 0});
    }
    result.frame = reserve(extent, entry ? sizeof(FC_NativeFrame) : kWord);
    result.xmm = reserve(extent, kSimdCount * 16, 16);

    if (entry) {
        result.values = reserve(extent, call.argument_count * kWord);
        for (std::uint32_t index = 0; index < call.argument_count; ++index) {
            const auto &value = call.arguments[index].value;
            // Captured entry values need slack for their actual alignment, even beyond platform stack alignment.
            result.buffers.push_back(reserve(extent, std::uint64_t{value.size} + value.alignment - 1));
        }
    }

    result.size = reserve(extent, 0, 16);
#if defined(_M_X64)
    result.size += 8; // The return address leaves the incoming stack eight bytes below a 16-byte boundary.
#endif

    return result;
}

} // namespace bridge_detail

using namespace bridge_detail;

std::expected<void, std::string> validate_bridge_layout(const FC_NativeCall &call) {
    // Hidden output pointers represent record results; compiler record conventions require an explicit storage description.
    if ((call.flags & FC_CALL_VARIADIC) || (hidden_result(call) && call.result.kind != FC_NATIVE_RECORD)) {
        return std::unexpected("Native bridges do not support variadic calls; hidden results must be records");
    }
    if ((call.flags & FC_CALL_COMPILER) && (call.result.kind == FC_NATIVE_RECORD ||
                                            std::ranges::any_of(std::span{call.arguments, call.argument_count}, [](const auto &argument) {
                                                return argument.value.kind == FC_NATIVE_RECORD;
                                            }))) {
        return std::unexpected("Native bridge records require an explicit custom call description");
    }
    // Stack displacements use signed 32-bit offsets, while x86 RET encodes callee cleanup in an unsigned 16-bit operand.
    if (call.argument_count > static_cast<std::uint32_t>(INT32_MAX) / kWord || call.stack_size > INT32_MAX - kSaved - 64 ||
        call.stack_size % kWord != 0 || (kWord == 8 && call.cleanup != FC_STACK_CLEANUP_NONE) ||
        (kWord == 4 && ((call.stack_size == 0 && call.cleanup != FC_STACK_CLEANUP_NONE) ||
                        (call.stack_size != 0 && call.cleanup != FC_STACK_CLEANUP_CALLER && call.cleanup != FC_STACK_CLEANUP_CALLEE))) ||
        (call.cleanup == FC_STACK_CLEANUP_CALLEE && call.stack_size > UINT16_MAX)) {
        return std::unexpected("Native bridge argument count, stack size or stack cleanup is unsupported");
    }

    // Check a value's physical location, including the distinct rules for results and hidden output pointers.
    const auto valid_storage = [&](const FC_NativeStorage &storage, const FC_NativeValue &value, bool result, bool hidden) {
        // Hidden results place a pointer in physical storage rather than the record's bytes.
        const auto size = hidden ? kWord : value.size;
        if (storage.kind == FC_NATIVE_STORAGE_STACK) {
            return (!result || hidden) && storage.register_id == FC_REGISTER_NONE && storage.stack_offset <= call.stack_size &&
                   size <= call.stack_size - storage.stack_offset && storage.stack_offset % std::min(value.alignment, kWord) == 0;
        }
        if (storage.kind != FC_NATIVE_STORAGE_REGISTER || storage.stack_offset != 0) {
            return false;
        }
        if (hidden) {
            return general(storage.register_id).has_value();
        }
        if (value.kind == FC_NATIVE_FLOAT_32 || value.kind == FC_NATIVE_FLOAT_64) {
            return simd(storage.register_id).has_value() || (result && kWord == 4 && storage.register_id == FC_REGISTER_ST0);
        }
        // x86 eight-byte integer or record results use EDX:EAX; other general-register values fit in one word.
        return general(storage.register_id).has_value() &&
               (size <= kWord || (result && kWord == 4 && size == 8 && storage.register_id == FC_REGISTER_EAX &&
                                  (value.kind == FC_NATIVE_INTEGER || value.kind == FC_NATIVE_RECORD)));
    };

    // Registers overlap by identity; stack values overlap by byte range, with widened arithmetic for range ends.
    const auto overlap = [&](const FC_NativeStorage &left, std::uint32_t left_size, const FC_NativeStorage &right,
                             std::uint32_t right_size) {
        if (left.kind != right.kind) {
            return false;
        }
        if (left.kind == FC_NATIVE_STORAGE_REGISTER) {
            return left.register_id == right.register_id;
        }
        return left.stack_offset < static_cast<std::uint64_t>(right.stack_offset) + right_size &&
               right.stack_offset < static_cast<std::uint64_t>(left.stack_offset) + left_size;
    };

    // Direct results return through registers; only a hidden result pointer can occupy a stack argument slot.
    if (call.result.kind == FC_NATIVE_VOID) {
        if (call.return_storage.kind != FC_NATIVE_STORAGE_NONE || call.return_storage.register_id != FC_REGISTER_NONE ||
            call.return_storage.stack_offset) {
            return std::unexpected("Void native bridge results require empty storage");
        }
    } else if (!valid_storage(call.return_storage, call.result, true, hidden_result(call))) {
        return std::unexpected("Native bridge result storage is unsupported");
    }

    // Each physical input needs exclusive storage, including the pointer supplied for a hidden result.
    for (std::uint32_t index = 0; index < call.argument_count; ++index) {
        const auto &argument = call.arguments[index];
        if (!valid_storage(argument.storage, argument.value, false, false)) {
            return std::unexpected("Native bridge argument value or storage is unsupported");
        }
        const auto size = argument.value.size;
        if (hidden_result(call) && overlap(argument.storage, size, call.return_storage, kWord)) {
            return std::unexpected("The native bridge's hidden result pointer shares storage with an argument");
        }
        for (std::uint32_t prior = 0; prior < index; ++prior) {
            const auto &other = call.arguments[prior];
            if (overlap(argument.storage, size, other.storage, other.value.size)) {
                return std::unexpected("Native bridge arguments use overlapping register or stack locations");
            }
        }
    }

    return {};
}

} // namespace fc::runtime::native
