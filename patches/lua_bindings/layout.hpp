#pragma once

#include <fc/native.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace fc::core_plugin::lua_bindings {

// Records the instructions used to verify a Lua function or lifecycle location in the executable.
struct NativeSite {
    // Byte offset from the executable's image base.
    std::uint32_t rva;
    std::string_view name;
    // Complete instructions from the supported executable, before rebasing any absolute operand.
    std::span<const std::uint8_t> bytes;
    // Byte offset of the four-byte absolute operand rewritten for the selected loaded image.
    std::size_t operand_offset{};
    // The referenced field's RVA; zero preserves the original instruction bytes.
    std::uint32_t target_rva{};
};

// Locates one executable's Lua helpers, lifecycle functions and state fields.
struct Layout {
    static constexpr std::size_t kFunctionCount = 35;

    std::string_view profile;
    // Helpers follow the function pointer order in FcLuaNative.
    std::span<const NativeSite, kFunctionCount> functions;
    NativeSite setup, registration, teardown, update;
    // Instructions that verify the global state pointer and the stack field offset.
    NativeSite state_reference, stack_reference;
    // RVAs of the global Lua state pointer and the one-byte initialization flag.
    std::uint32_t state, initialized;
};

// Returns the Lua layout for the selected executable profile, or nullptr when unsupported.
const Layout *select_layout(std::string_view profile) noexcept;

// Copies the expected instructions, rebasing any absolute operand to image_base.
fc::Evidence instruction_evidence(const NativeSite &site, std::uintptr_t image_base);

} // namespace fc::core_plugin::lua_bindings
