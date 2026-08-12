#pragma once

#include <cstdint>

namespace fusioncutter::patches::rcon_server::gog {

// Calls the game's RCON manager through its mixed register/stack x86 ABI.
[[nodiscard]] std::uint32_t execute_native_command(std::uintptr_t function, std::int32_t response_output,
                                                   const wchar_t* command, std::uint32_t sender,
                                                   std::uint32_t message_type) noexcept;

} // namespace fusioncutter::patches::rcon_server::gog
