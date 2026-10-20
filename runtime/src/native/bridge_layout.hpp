#pragma once

#include "bridge.hpp"

#include <array>
#include <optional>
#include <vector>

namespace fc::runtime::native::bridge_detail {

// Register sets, home space and saved words follow the executing platform ABI.
#if defined(_M_X64)
inline constexpr std::uint32_t kWord = 8;
inline constexpr std::array<std::uint8_t, 15> kGeneral{0, 1, 2, 3, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
inline constexpr std::uint32_t kSimdCount = 16;
inline constexpr std::uint32_t kHome = 32;
#else
inline constexpr std::uint32_t kWord = 4;
inline constexpr std::array<std::uint8_t, 7> kGeneral{0, 1, 2, 3, 5, 6, 7};
inline constexpr std::uint32_t kSimdCount = 8;
inline constexpr std::uint32_t kHome = 0;
#endif

// Pushed general registers plus one EFLAGS/RFLAGS word, in bytes.
inline constexpr std::uint32_t kSaved = (static_cast<std::uint32_t>(kGeneral.size()) + 1) * kWord;

// Reports whether physical result storage holds a pointer to the logical output object.
bool hidden_result(const FC_NativeCall &);
// Translate ABI register IDs to instruction encoding numbers; unsupported IDs return nullopt.
std::optional<std::uint8_t> general(std::uint32_t register_id);
// Returns the XMM index available on this architecture, or nullopt for another register.
std::optional<std::uint8_t> simd(std::uint32_t register_id);

// Assigns private stack regions used while converting between physical calls and logical frames.
struct FrameLayout {
    // size is the private allocation extent in bytes; other offsets are relative to its aligned stack base.
    // anchor stores saved x86 ESP; target stores the callee address.
    std::uint32_t size{}, anchor{}, target{};
    // Byte offsets for the logical output pointer, saved EFLAGS/RFLAGS, and the invoker caller's flags pointer, respectively.
    std::uint32_t result_pointer{}, flags{}, flag_pointer{};
    // Direct result bytes and entry argument pointers; frame holds an entry FC_NativeFrame or a borrowed invoker frame pointer.
    // xmm locates the saved 16-byte XMM registers.
    std::uint32_t result{}, values{}, frame{}, xmm{};
    // Entry-only offsets with extra space to align each captured logical argument.
    std::vector<std::uint32_t> buffers;
};

// Assign private stack offsets for a call valid for validate_call(call, true); oversized layouts throw length_error.
// Entries reserve callback space; invokers reserve the described outgoing argument area.
FrameLayout layout(const FC_NativeCall &, bool entry);

} // namespace fc::runtime::native::bridge_detail
