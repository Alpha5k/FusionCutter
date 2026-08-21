#include <cstdint>

// The distinct value and complete signature let the loader driver prove all forwarding reaches the real entry.
extern "C" std::uint32_t GameWinMain(std::uint64_t argument1, std::uint64_t argument2, std::uint64_t argument3,
                                     std::uint64_t argument4, std::uint8_t* argument5, std::uint32_t argument6,
                                     std::uint64_t argument7) noexcept {
    return static_cast<std::uint32_t>(argument1 + argument2 + argument3 + argument4 +
                                      reinterpret_cast<std::uintptr_t>(argument5) + argument6 + argument7);
}
