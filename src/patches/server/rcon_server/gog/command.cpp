#include "command.hpp"

namespace fusioncutter::patches::rcon_server::gog {

__declspec(naked) std::uint32_t execute_native_command(std::uintptr_t, std::int32_t, const wchar_t*, std::uint32_t,
                                                       std::uint32_t) noexcept {
    // The game reads its first two arguments from ECX/EDX but leaves both stack arguments for the caller to remove.
    // clang-format off
    __asm {
        mov  eax, dword ptr [esp + 0x04]
        mov  ecx, dword ptr [esp + 0x08]
        mov  edx, dword ptr [esp + 0x0C]
        push dword ptr [esp + 0x14]
        push dword ptr [esp + 0x14]
        call eax
        add  esp, 8
        ret
    }
    // clang-format on
}

} // namespace fusioncutter::patches::rcon_server::gog
