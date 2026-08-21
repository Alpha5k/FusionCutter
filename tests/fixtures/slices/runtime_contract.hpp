#pragma once

#include "runtime_contract.h"

#include <FusionCutter/SDK.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>

namespace fc::fixtures::runtime {

// The copied interface crosses plugin DLL boundaries through an SDK-generated context and C calling convention thunk.
struct RuntimeServiceV1 {
    static constexpr std::string_view id = "RuntimeServiceV1";
    fc::InterfaceFunction<std::int32_t(std::int32_t) noexcept> evaluate;
};

// The logical typed site uses different reviewed register and stack homes on each supported architecture.
#if defined(_M_IX86)
using TypedSite = fc::NativeCall<std::int32_t(std::int32_t, std::int32_t, std::int32_t),
                                 fc::abi::x86<fc::abi::args<fc::abi::ecx, fc::abi::edx, fc::abi::stack<0>>,
                                              fc::abi::result<fc::abi::eax>, fc::abi::callee_cleanup>>;
#else
using TypedSite =
    fc::NativeCall<std::int32_t(std::int32_t, std::int32_t, std::int32_t, std::int32_t, std::int32_t),
                   fc::abi::x64<fc::abi::args<fc::abi::rcx, fc::abi::rdx, fc::abi::r8, fc::abi::r9, fc::abi::stack<0>>,
                                fc::abi::result<fc::abi::rax>>>;
#endif

// The late peer site uses an ordinary C boundary because its one argument needs no expanded native layout.
using LateSite = std::int32_t(FC_CALL*)(std::int32_t) noexcept;

// Short instruction prefixes reject a different fixture build without coupling plans to relocated addresses.
[[nodiscard]] inline fc::Evidence typed_site_evidence() {
#if defined(_M_IX86)
    return fc::exact_bytes(std::array{std::byte{0x55}, std::byte{0x8b}, std::byte{0xec}});
#else
    return fc::exact_bytes(std::array{std::byte{0x44}, std::byte{0x89}, std::byte{0x4c}});
#endif
}

[[nodiscard]] inline fc::Evidence instruction_site_evidence() {
#if defined(_M_IX86)
    return fc::exact_bytes(std::array{std::byte{0x55}, std::byte{0x8b}, std::byte{0xec}});
#else
    return fc::exact_bytes(std::array{std::byte{0x48}, std::byte{0x8d}, std::byte{0x05}});
#endif
}

[[nodiscard]] inline fc::Evidence late_site_evidence() {
    return fc::exact_bytes(std::array{std::byte{0x55}, std::byte{0x8b}, std::byte{0xec}});
}

static_assert(std::is_standard_layout_v<RuntimeServiceV1>);
static_assert(std::is_trivially_copyable_v<RuntimeServiceV1>);
static_assert(std::is_standard_layout_v<FC_SliceHostState>);
static_assert(std::is_trivially_copyable_v<FC_SliceHostState>);

} // namespace fc::fixtures::runtime
