#pragma once

#include <algorithm>
#include <cstddef>
#include <string_view>

namespace fusioncutter::patches::rcon_server {

template <typename Character>
[[nodiscard]] std::basic_string_view<Character> bounded_string_view(const Character* text,
                                                                    std::size_t capacity) noexcept {
    if (text == nullptr) {
        return {};
    }

    const auto end = std::find(text, text + capacity, Character{});
    return {text, static_cast<std::size_t>(end - text)};
}

} // namespace fusioncutter::patches::rcon_server
