#include <cstdint>
#include <atomic>

namespace {

std::atomic_bool valid{true};
}

// Checks all seven forwarded launcher arguments and returns the fixture's fixed result even on a mismatch.
extern "C" std::uint32_t GameWinMain(std::uint64_t first, std::uint64_t second, std::uint64_t third, std::uint64_t fourth,
                                     std::uint8_t *fifth, std::uint32_t sixth, std::uint64_t seventh) noexcept {
    // Wide values and a nonzero pointer expose truncated or reordered arguments at the native entry.
    if (first != 0x1234567800000001ull || second != 0x2345678900000002ull || third != 0x3456789A00000003ull ||
        fourth != 0x456789AB00000004ull || reinterpret_cast<std::uintptr_t>(fifth) != 0x1003 || sixth != 0xFEDCBA98 ||
        seventh != 0x56789ABC00000006ull) {
        valid.store(false);
    }
    return 0x87654321;
}

// Returns whether every call so far matched the expected arguments; true before the first call.
extern "C" bool FC_Test_OriginalValid() noexcept {
    return valid.load();
}
