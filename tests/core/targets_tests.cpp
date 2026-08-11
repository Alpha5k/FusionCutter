#include "recognition.hpp"
#include "layouts/profiles.hpp"

#include <catch2/catch_test_macros.hpp>

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::uint32_t kImageSize = 0x4000;
constexpr std::uint32_t kHeadersSize = 0x400;
constexpr std::uint32_t kMarkerRva = 0x1100;
constexpr std::uint32_t kTimestamp = 0x12345678;
constexpr std::uint32_t kCatalogTimestamp = 0xC0DEC0DE;
constexpr std::array kMarker = {std::byte{0xDE}, std::byte{0xAD}, std::byte{0xBE}, std::byte{0xEF}};

#if defined(_M_IX86)
constexpr auto kArchitecture = fusioncutter::Architecture::X86;
using OptionalHeader = IMAGE_OPTIONAL_HEADER32;
constexpr WORD kMachine = IMAGE_FILE_MACHINE_I386;
constexpr WORD kOptionalMagic = IMAGE_NT_OPTIONAL_HDR32_MAGIC;
#elif defined(_M_X64)
constexpr auto kArchitecture = fusioncutter::Architecture::X64;
using OptionalHeader = IMAGE_OPTIONAL_HEADER64;
constexpr WORD kMachine = IMAGE_FILE_MACHINE_AMD64;
constexpr WORD kOptionalMagic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
#else
#error Unsupported target-test architecture
#endif

template <typename T> void write_value(std::vector<std::byte>& image, std::size_t offset, const T& value) {
    REQUIRE(offset <= image.size());
    REQUIRE(sizeof(value) <= image.size() - offset);
    std::memcpy(image.data() + offset, &value, sizeof(value));
}

[[nodiscard]] IMAGE_SECTION_HEADER make_section(std::string_view name, std::uint32_t rva, std::uint32_t size) {
    REQUIRE(name.size() <= IMAGE_SIZEOF_SHORT_NAME);

    IMAGE_SECTION_HEADER section{};
    std::memcpy(section.Name, name.data(), name.size());
    section.VirtualAddress = rva;
    section.Misc.VirtualSize = size;
    return section;
}

constexpr std::array kSections = {
    fusioncutter::targets::SectionProfile{".text", 0x1000, 0x1200},
    fusioncutter::targets::SectionProfile{".rdata", 0x3000, 0x500},
};

[[nodiscard]] std::vector<std::byte>
make_mapped_image(std::uint32_t image_size, std::uint32_t timestamp,
                  std::span<const fusioncutter::targets::SectionProfile> sections,
                  std::optional<fusioncutter::targets::ImageMarker> marker = std::nullopt) {
    REQUIRE(image_size >= kHeadersSize);
    REQUIRE(sections.size() <= (std::numeric_limits<WORD>::max)());

    std::vector<std::byte> image(image_size);

    IMAGE_DOS_HEADER dos_header{};
    dos_header.e_magic = IMAGE_DOS_SIGNATURE;
    dos_header.e_lfanew = 0x80;
    write_value(image, 0, dos_header);

    constexpr DWORD kSignature = IMAGE_NT_SIGNATURE;
    const auto nt_offset = static_cast<std::size_t>(dos_header.e_lfanew);
    write_value(image, nt_offset, kSignature);

    IMAGE_FILE_HEADER file_header{};
    file_header.Machine = kMachine;
    file_header.NumberOfSections = static_cast<WORD>(sections.size());
    file_header.TimeDateStamp = timestamp;
    file_header.SizeOfOptionalHeader = static_cast<WORD>(sizeof(OptionalHeader));
    const auto file_header_offset = nt_offset + sizeof(kSignature);
    write_value(image, file_header_offset, file_header);

    OptionalHeader optional_header{};
    optional_header.Magic = kOptionalMagic;
    optional_header.SizeOfImage = image_size;
    optional_header.SizeOfHeaders = kHeadersSize;
    const auto optional_header_offset = file_header_offset + sizeof(file_header);
    write_value(image, optional_header_offset, optional_header);

    const auto section_table_offset = optional_header_offset + sizeof(optional_header);
    REQUIRE(section_table_offset <= kHeadersSize);
    REQUIRE(sections.size() * sizeof(IMAGE_SECTION_HEADER) <= kHeadersSize - section_table_offset);
    for (std::size_t index = 0; index < sections.size(); ++index) {
        const auto& section = sections[index];
        write_value(image, section_table_offset + index * sizeof(IMAGE_SECTION_HEADER),
                    make_section(section.name, section.virtual_address, section.virtual_size));
    }

    if (marker.has_value()) {
        const auto offset = static_cast<std::size_t>(marker->rva);
        REQUIRE(offset <= image.size());
        REQUIRE(marker->expected.size() <= image.size() - offset);
        std::copy(marker->expected.begin(), marker->expected.end(),
                  image.begin() + static_cast<std::ptrdiff_t>(offset));
    }
    return image;
}

[[nodiscard]] std::vector<std::byte> make_mapped_image() {
    return make_mapped_image(kImageSize, kTimestamp, kSections,
                             fusioncutter::targets::ImageMarker{kMarkerRva, kMarker});
}

[[nodiscard]] std::vector<std::byte> make_mapped_image(const fusioncutter::targets::ImageProfile& profile) {
    return make_mapped_image(profile.size_of_image, profile.timestamp.value_or(kCatalogTimestamp), profile.sections,
                             profile.marker);
}

[[nodiscard]] fusioncutter::targets::ImageProfile make_profile(std::string_view fingerprint,
                                                               std::span<const std::byte> marker = kMarker,
                                                               std::uint32_t image_size = kImageSize) {
    return {
        fingerprint,
        fusioncutter::TargetLayout::SteamRetail,
        fusioncutter::TargetImage::Game,
        "BattlefrontII.exe",
        kArchitecture,
        image_size,
        kTimestamp,
        kSections,
        fusioncutter::targets::ImageMarker{kMarkerRva, marker},
    };
}

} // namespace

TEST_CASE("Known mapped image is recognized without modification", "[core][targets]") {
    auto image = make_mapped_image();
    const auto image_before = image;
    constexpr std::array wrong_marker = {std::byte{0x00}, std::byte{0x11}, std::byte{0x22}, std::byte{0x33}};
    const std::array profiles = {
        make_profile("synthetic-steam"),
        make_profile("synthetic-other", wrong_marker),
    };

    const auto base = reinterpret_cast<std::uintptr_t>(image.data());
    const auto result = fusioncutter::targets::recognize_mapped_image(
        "BATTLEFRONTII.EXE", fusioncutter::HostRole::Client, base, image, profiles);

    REQUIRE(result.has_value());
    CHECK(std::string(result->fingerprint) == "synthetic-steam");
    CHECK(result->context.layout == fusioncutter::TargetLayout::SteamRetail);
    CHECK(result->context.role == fusioncutter::HostRole::Client);
    CHECK(result->context.image.identity == fusioncutter::TargetImage::Game);
    CHECK(result->context.image.architecture == kArchitecture);
    CHECK(result->context.image.base == base);
    CHECK(result->context.image.size == kImageSize);

    using SyntheticFunction = void (*)();
    CHECK(result->context.image.address_at_rva(kMarkerRva, kMarker.size()) == base + kMarkerRva);
    CHECK(result->context.image.address_at_rva(kImageSize) == 0);
    CHECK(result->context.image.function_at_rva<SyntheticFunction>(kMarkerRva) != nullptr);
    CHECK(result->context.image.function_at_rva<SyntheticFunction>(kImageSize) == nullptr);

    const auto* marker = result->context.image.read_at_rva<std::array<std::byte, kMarker.size()>>(kMarkerRva);
    REQUIRE(marker != nullptr);
    CHECK(std::memcmp(marker->data(), kMarker.data(), kMarker.size()) == 0);
    CHECK(result->context.image.mutable_at_rva<std::array<std::byte, kMarker.size()>>(kMarkerRva) == marker);
    CHECK(result->context.image.read_at_rva<std::uint32_t>(kImageSize - 2) == nullptr);
    CHECK(result->context.image.mutable_at_rva<std::uint32_t>(kImageSize - 2) == nullptr);
    CHECK(std::ranges::equal(image, image_before));
}

TEST_CASE("Every compiled production profile is recognized without ambiguity", "[core][targets]") {
    const auto profiles = fusioncutter::targets::layouts::known_image_profiles();
    REQUIRE_FALSE(profiles.empty());

    for (const auto& profile : profiles) {
        INFO("profile=" << std::string(profile.fingerprint) << " basename=" << std::string(profile.basename));
        REQUIRE(profile.architecture == kArchitecture);

        auto image = make_mapped_image(profile);
        const auto base = reinterpret_cast<std::uintptr_t>(image.data());
        const auto result = fusioncutter::targets::recognize_mapped_image(
            profile.basename, fusioncutter::HostRole::Client, base, image, profiles);

        REQUIRE(result.has_value());
        CHECK(std::string(result->fingerprint) == std::string(profile.fingerprint));
        CHECK(result->context.layout == profile.layout);
        CHECK(result->context.image.identity == profile.identity);
        CHECK(result->context.image.architecture == profile.architecture);
        CHECK(result->context.image.base == base);
        CHECK(result->context.image.size == profile.size_of_image);
    }
}

TEST_CASE("Unrecognized, ambiguous, and malformed images yield no context or writes", "[core][targets]") {
    SECTION("unknown") {
        auto image = make_mapped_image();
        const auto image_before = image;
        const std::array profiles = {make_profile("wrong-size", kMarker, kImageSize + 0x1000)};

        const auto result = fusioncutter::targets::recognize_mapped_image(
            "BattlefrontII.exe", fusioncutter::HostRole::Client, reinterpret_cast<std::uintptr_t>(image.data()), image,
            profiles);

        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().kind == fusioncutter::targets::RecognitionErrorKind::UnknownImage);
        CHECK(std::ranges::equal(image, image_before));
    }

    SECTION("ambiguous") {
        auto image = make_mapped_image();
        const auto image_before = image;
        const std::array profiles = {make_profile("first"), make_profile("second")};

        const auto result = fusioncutter::targets::recognize_mapped_image(
            "BattlefrontII.exe", fusioncutter::HostRole::Server, reinterpret_cast<std::uintptr_t>(image.data()), image,
            profiles);

        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().kind == fusioncutter::targets::RecognitionErrorKind::AmbiguousImage);
        CHECK(std::ranges::equal(image, image_before));
    }

    SECTION("malformed") {
        auto image = make_mapped_image();
        image.front() = std::byte{0};
        const auto image_before = image;
        const std::array profiles = {make_profile("unreachable")};

        const auto result = fusioncutter::targets::recognize_mapped_image(
            "BattlefrontII.exe", fusioncutter::HostRole::Client, reinterpret_cast<std::uintptr_t>(image.data()), image,
            profiles);

        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().kind == fusioncutter::targets::RecognitionErrorKind::MalformedImage);
        CHECK(std::ranges::equal(image, image_before));
    }
}
