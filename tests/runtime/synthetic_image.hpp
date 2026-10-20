#pragma once

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

namespace fc::testing {

constexpr std::size_t kNtOffset = 0x80;
constexpr std::size_t kFileHeaderOffset = kNtOffset + 4;
constexpr std::size_t kOptionalHeaderOffset = kFileHeaderOffset + 20;
constexpr std::uint16_t kMachine = sizeof(void *) == 8 ? 0x8664 : 0x014c;
constexpr std::uint16_t kOptionalMagic = sizeof(void *) == 8 ? 0x020b : 0x010b;
constexpr std::uint16_t kOptionalHeaderSize = sizeof(void *) == 8 ? 0x00f0 : 0x00e0;
constexpr std::size_t kSectionTableOffset = kOptionalHeaderOffset + kOptionalHeaderSize;

// Writes one field into the PE fixture at a byte offset, failing the test if it exceeds the buffer.
template <class T> void put(std::vector<std::byte> &bytes, std::size_t offset, T value) {
    REQUIRE(offset <= bytes.size());
    REQUIRE(sizeof(value) <= bytes.size() - offset);

    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

// Writes a PE section name of at most eight bytes; the caller supplies enough storage for the name.
// Short names rely on the buffer's existing zero padding.
inline void put_name(std::vector<std::byte> &bytes, std::size_t offset, std::string_view value) {
    REQUIRE(value.size() <= 8);

    std::memcpy(bytes.data() + offset, value.data(), value.size());
}

// Holds exact PE bytes and the independent facts tests expect the parser to return.
struct SyntheticImage {
    std::vector<std::byte> bytes;
    std::uint32_t image_size{0x5000};
    std::uint32_t text_rva{0x1000};
    std::uint32_t text_size{0x180};
    std::uint32_t data_rva{0x3000};
    std::uint32_t data_size{0x80};
};

// Creates a minimal PE file for this test process's architecture, with distinct raw offsets and RVAs.
inline SyntheticImage make_image() {
    SyntheticImage image;
    image.bytes.resize(0x800);

    // Build the minimal DOS, file, and optional headers needed by the production parser.
    put(image.bytes, 0, std::uint16_t{0x5a4d});
    put(image.bytes, 0x3c, std::uint32_t{kNtOffset});
    put(image.bytes, kNtOffset, std::uint32_t{0x00004550});
    put(image.bytes, kFileHeaderOffset, kMachine);
    put(image.bytes, kFileHeaderOffset + 2, std::uint16_t{2});
    put(image.bytes, kFileHeaderOffset + 16, kOptionalHeaderSize);
    put(image.bytes, kOptionalHeaderOffset, kOptionalMagic);

    // PE32+ stores a 64-bit ImageBase at offset 24; PE32 stores a 32-bit ImageBase at offset 28.
#if defined(_WIN64)
    put(image.bytes, kOptionalHeaderOffset + 24, std::uint64_t{0x140000000});
#else
    put(image.bytes, kOptionalHeaderOffset + 28, std::uint32_t{0x400000});
#endif
    put(image.bytes, kOptionalHeaderOffset + 56, image.image_size);
    put(image.bytes, kOptionalHeaderOffset + 60, std::uint32_t{0x400});

    // Raw offsets differ from RVAs so the test catches confusion between file and mapped addresses.
    put_name(image.bytes, kSectionTableOffset, ".text");
    put(image.bytes, kSectionTableOffset + 8, image.text_size);
    put(image.bytes, kSectionTableOffset + 12, image.text_rva);
    put(image.bytes, kSectionTableOffset + 16, std::uint32_t{0x200});
    put(image.bytes, kSectionTableOffset + 20, std::uint32_t{0x400});
    put(image.bytes, kSectionTableOffset + 36, std::uint32_t{0x60000020});

    const auto data = kSectionTableOffset + 40;
    put_name(image.bytes, data, ".data");
    put(image.bytes, data + 8, image.data_size);
    put(image.bytes, data + 12, image.data_rva);
    put(image.bytes, data + 16, std::uint32_t{0x100});
    put(image.bytes, data + 20, std::uint32_t{0x600});
    put(image.bytes, data + 36, std::uint32_t{0xc0000040});

    return image;
}

} // namespace fc::testing
