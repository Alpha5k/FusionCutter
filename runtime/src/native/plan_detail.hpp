#pragma once

#include "plan.hpp"

#include <Zydis/Zydis.h>

#include <concepts>
#include <limits>
#include <type_traits>
#include <utility>

namespace fc::runtime::plan_detail {

// Decode and range checks use the executing machine's address and instruction domain.
#if defined(_WIN64)
inline constexpr auto kArchitecture = FC_ARCH_X64;
#else
inline constexpr auto kArchitecture = FC_ARCH_X86;
#endif

inline constexpr auto kMaximum = std::numeric_limits<std::size_t>::max();

// Turns expected failures into Error exceptions for the enclosing plan operation's catch boundary.
template <class T, class E> T take(std::expected<T, E> value) {
    if (!value) {
        if constexpr (std::same_as<E, Error>) {
            throw value.error();
        } else {
            throw Error{value.error()};
        }
    }
    if constexpr (!std::is_void_v<T>) {
        return std::move(*value);
    }
}

// Converts a failed displacement encoding into the plan's usual error path.
template <class T> T take(std::optional<T> value) {
    if (!value) {
        throw Error{"Native displacement is outside its encoding range"};
    }
    return *value;
}

// Copy ABI views into owned storage; nonempty views require a valid data pointer.
std::string copy_text(FC_Text);
std::vector<std::byte> copy_bytes(FC_Bytes);
// Checks a nonempty byte range's power-of-two alignment, aligned address and representable end address.
void range(std::uint64_t size, std::uint32_t alignment, std::uintptr_t address);
// Returns the bytes needed to check byte, pointer or direct-branch evidence.
std::size_t extent(const EvidenceRecord &);

// Encodes a scalar or resolved address for a field; relative addresses use the instruction or field end.
// Throws Error when the source signedness, decoded encoding or architecture cannot represent the value.
std::uint64_t encode_field(std::uint32_t size, bool encoded_signed, std::uint64_t value, bool signed_value, bool address_value,
                           bool relative, std::uintptr_t end, FC_Architecture architecture);

// Holds one instruction and its operands decoded for the executing architecture.
struct Decoded {
    ZydisDecodedInstruction instruction{};
    ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT]{};
};

// Decodes one instruction at the start of the supplied bytes or throws Error.
Decoded decode(std::span<const std::byte>);
// Copies whole instructions covering minimum bytes; exact rejects an edit ending inside an instruction.
std::vector<std::byte> instructions(const SubmittedPlan &, LocationRecord, std::size_t minimum, bool exact);
// Requires one five-byte direct CALL or JMP and resolves its signed displacement to an absolute address.
std::uintptr_t branch_target(std::span<const std::byte>, std::uintptr_t address, std::uint32_t kind);

} // namespace fc::runtime::plan_detail
