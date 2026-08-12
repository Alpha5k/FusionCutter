#include "gog/command.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

namespace rcon = fusioncutter::patches::rcon_server::gog;

namespace {

struct CommandObservation {
    std::int32_t response_output{};
    const wchar_t* command{};
    std::uint32_t sender{};
    std::uint32_t message_type{};
};

CommandObservation g_observation;

__declspec(naked) std::uint32_t synthetic_native_command() {
    // Keep the synthetic callee's observed register and stack positions visually aligned.
    // clang-format off
    __asm {
        mov  g_observation.response_output, ecx
        mov  g_observation.command, edx
        mov  eax, dword ptr [esp + 4]
        mov  g_observation.sender, eax
        mov  eax, dword ptr [esp + 8]
        mov  g_observation.message_type, eax
        mov  eax, 0x12345678
        ret
    }
    // clang-format on
}

__declspec(naked) std::uintptr_t current_stack_pointer() {
    __asm {
        mov eax, esp
        ret
    }
}

} // namespace

TEST_CASE("GOG RCON preserves the native command ABI") {
    constexpr auto kCommand = L"/status";
    const auto stack_before = current_stack_pointer();
    const auto result = rcon::execute_native_command(reinterpret_cast<std::uintptr_t>(&synthetic_native_command), -1,
                                                     kCommand, 0x11223344, 0x55667788);
    const auto stack_after = current_stack_pointer();

    CHECK(result == 0x12345678);
    CHECK(g_observation.response_output == -1);
    CHECK(g_observation.command == kCommand);
    CHECK(g_observation.sender == 0x11223344);
    CHECK(g_observation.message_type == 0x55667788);
    CHECK(stack_after == stack_before);
}
