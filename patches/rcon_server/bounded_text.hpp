#pragma once

#include <cstddef>
#include <string_view>

namespace fc::core_plugin::rcon_server {

// Borrows text up to its first null or capacity characters; text must be readable through that scan and outlive the view.
// Null input returns an empty view; otherwise size == capacity means no terminating null was found.
template <class Character> std::basic_string_view<Character> bounded_text(const Character *text, std::size_t capacity) noexcept {
    if (text == nullptr) {
        return {};
    }

    std::size_t length{};
    while (length < capacity && text[length] != Character{}) {
        ++length;
    }
    return {text, length};
}

} // namespace fc::core_plugin::rcon_server
