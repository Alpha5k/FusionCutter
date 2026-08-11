#include "game_layout.hpp"

#include <cstring>

namespace fusioncutter::patches::direct_transport {
namespace {

template <std::size_t Size, std::size_t OffsetCount>
void relocate_addresses(std::array<std::byte, Size>& bytes, const std::array<std::size_t, OffsetCount>& offsets,
                        std::uintptr_t image_base) noexcept {
    for (const auto offset : offsets) {
        std::uint32_t preferred{};
        std::memcpy(&preferred, bytes.data() + offset, sizeof(preferred));
        const auto rva = preferred - kPreferredImageBase;
        const auto relocated = static_cast<std::uint32_t>(image_base + rva);
        std::memcpy(bytes.data() + offset, &relocated, sizeof(relocated));
    }
}

} // namespace

std::array<std::byte, 21> relocated_getter_preimage(const GameLayout& layout, std::uintptr_t image_base,
                                                    bool networking) noexcept {
    auto bytes = networking ? layout.get_networking_preimage : layout.get_matchmaking_preimage;
    relocate_addresses(bytes, std::array<std::size_t, 2>{2, 13}, image_base);
    return bytes;
}

std::array<std::byte, 16> relocated_packet_preimage(const GameLayout& layout, std::uintptr_t image_base,
                                                    bool initialize) noexcept {
    auto bytes = initialize ? layout.packet_initialize_preimage : layout.packet_allocate_preimage;
    if (initialize) {
        relocate_addresses(bytes, std::array<std::size_t, 1>{10}, image_base);
    } else {
        relocate_addresses(bytes, std::array<std::size_t, 2>{4, 9}, image_base);
    }
    return bytes;
}

} // namespace fusioncutter::patches::direct_transport
