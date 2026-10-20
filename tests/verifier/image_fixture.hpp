#pragma once

#include "images/image.hpp"
#include "../runtime/synthetic_image.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ranges>

namespace fc::tests::verifier {

// Writes a PE fixture matching a production BF2 profile, with NOP instructions and one data byte.
// The fixture exercises file loading and planning; actual game bytes are checked separately.
inline std::filesystem::path write_image(const std::filesystem::path &directory) {
    const auto catalog = runtime::known_image_profiles();
    const auto found = std::ranges::find_if(catalog, [](const auto &profile) {
        return profile.id.starts_with("BF2.");
    });
    REQUIRE(found != catalog.end());

    auto bytes = testing::make_image().bytes;
    bytes.resize(0xa00);
    std::fill(bytes.begin() + testing::kSectionTableOffset, bytes.end(), std::byte{});
    testing::put(bytes, testing::kFileHeaderOffset + 2, static_cast<std::uint16_t>(found->sections.size()));
    testing::put(bytes, testing::kOptionalHeaderOffset + 56, found->image_size);

    // Small raw sections expand to the catalog's virtual extents in the offline mapper.
    for (std::size_t index = 0; index != found->sections.size(); ++index) {
        const auto &section = found->sections[index];
        const auto header = testing::kSectionTableOffset + index * 40;
        const auto raw = static_cast<std::uint32_t>(0x400 + index * 0x200);
        testing::put_name(bytes, header, section.name);
        testing::put(bytes, header + 8, section.size);
        testing::put(bytes, header + 12, section.rva);
        testing::put(bytes, header + 16, std::uint32_t{0x200});
        testing::put(bytes, header + 20, raw);
        testing::put(bytes, header + 36, section.name == ".text" ? std::uint32_t{0x60000020} : std::uint32_t{0xc0000040});
        if (section.name == ".text") {
            std::fill(bytes.begin() + raw, bytes.begin() + raw + 0x200, std::byte{0x90});
        }
        if (section.name == ".data") {
            bytes[raw + 16] = std::byte{0x31};
        }
    }

    const auto path = directory / found->filenames.front();
    std::ofstream output{path, std::ios::binary};
    REQUIRE(output);
    output.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    REQUIRE(output);
    return path;
}

} // namespace fc::tests::verifier
