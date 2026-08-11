#pragma once

#include <cstdint>

namespace fusioncutter::tests {

struct GameWinMainArguments {
    std::uint64_t argument1;
    std::uint64_t argument2;
    std::uint64_t argument3;
    std::uint64_t argument4;
    std::uint8_t* argument5;
    std::uint32_t argument6;
    std::uint64_t argument7;
};

inline constexpr std::uint32_t kGameWinMainResult = 0xC0DE'C0DE;

} // namespace fusioncutter::tests
