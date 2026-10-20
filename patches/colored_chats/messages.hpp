#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace fc::core_plugin::colored_chats {

inline constexpr std::size_t kChatCapacity = 128;
inline constexpr std::uint32_t kAdminSender = 0x40;

// Holds opaque native AARRGGBB colors for the four chat message types.
struct Colors {
    std::uint32_t chat, team, admin, buddy;
};

// Converts exactly six hexadecimal RGB digits to an opaque native color.
std::optional<std::uint32_t> parse_color(std::string_view text) noexcept;

// Returns null unchanged, otherwise replaces newlines and caps text at 127 characters plus NUL.
// The returned pointer borrows buffer; the native queue copies it synchronously.
const wchar_t *one_line(const wchar_t *text, std::array<wchar_t, kChatCapacity> &buffer) noexcept;

// Selects admin, buddy, team or ordinary chat color, in that precedence order.
std::uint32_t message_color(const Colors &colors, std::uint32_t sender, std::uint32_t channel, bool buddy) noexcept;

} // namespace fc::core_plugin::colored_chats
