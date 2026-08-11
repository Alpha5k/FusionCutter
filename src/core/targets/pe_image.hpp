#pragma once

#include <FusionCutter/target.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>
#include <vector>

namespace fusioncutter::targets {

struct PeSectionFacts {
    std::array<char, 8> name;
    std::uint32_t virtual_address;
    std::uint32_t virtual_size;
};

struct PeImageFacts {
    Architecture architecture;
    std::uint32_t size_of_image;
    std::uint32_t timestamp;
    std::vector<PeSectionFacts> sections;
};

[[nodiscard]] std::expected<PeImageFacts, std::string_view> inspect_mapped_pe(std::span<const std::byte> mapped_image);

} // namespace fusioncutter::targets
