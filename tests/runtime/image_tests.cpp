#include "images/image.hpp"
#include "patches/selection.hpp"
#include "synthetic_image.hpp"

#include <catch2/catch_test_macros.hpp>

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace fc::testing;

// Gives the synthetic PE one profile with two accepted filenames.
// Keep this catalog alive at a fixed address while its profiles are used.
struct TestCatalog {
    std::array<std::string_view, 2> filenames{"fixture.exe", "FIXTURE-ALIAS.EXE"};
    std::array<fc::runtime::SectionProfile, 2> sections;
    std::array<fc::runtime::ImageProfile, 1> profiles;

    explicit TestCatalog(const SyntheticImage &image)
        : sections{fc::runtime::SectionProfile{".text", image.text_rva, image.text_size},
                   fc::runtime::SectionProfile{".data", image.data_rva, image.data_size}},
          profiles{fc::runtime::ImageProfile{"Synthetic_Profile", filenames, image.image_size, sections}} {}
};

// Checks whether an image diagnostic contains an expected message fragment.
bool error_contains(const fc::runtime::Diagnostic &error, std::string_view text) {
    return error.message.find(text) != std::string::npos;
}

// Reads fixture facts with an independent byte reader for comparison with production parsing.
template <class T> T independent_read(std::span<const std::byte> bytes, std::size_t offset) {
    REQUIRE(offset <= bytes.size());
    REQUIRE(sizeof(T) <= bytes.size() - offset);

    T value{};
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

// Reads the complete fixture file into bytes, failing the test if reading fails.
std::vector<std::byte> read_file(const std::filesystem::path &path) {
    std::ifstream input{path, std::ios::binary | std::ios::ate};
    REQUIRE(input);
    const auto length = input.tellg();
    REQUIRE(length >= 0);

    std::vector<std::byte> bytes(static_cast<std::size_t>(length));
    input.seekg(0);
    input.read(reinterpret_cast<char *>(bytes.data()), length);

    REQUIRE(input);
    return bytes;
}

// Owns a fixture profile and the filename and section names referenced by its non-owning views.
// Keep this object alive at a fixed address while any copied profile is used.
struct FixtureProfile {
    std::string filename;
    // Owns the names referenced by the SectionProfile records below.
    std::vector<std::string> section_names;
    std::vector<std::string_view> filenames;
    std::vector<fc::runtime::SectionProfile> sections;
    fc::runtime::ImageProfile profile;

    // Derives identifying PE facts from a valid fixture DLL without calling the production parser.
    explicit FixtureProfile(const std::filesystem::path &path) {
        const auto bytes = read_file(path);

        // Follow the DOS header's PE offset and SizeOfOptionalHeader to locate the section table.
        const auto pe_offset = independent_read<std::uint32_t>(bytes, 0x3c);
        const auto section_count = independent_read<std::uint16_t>(bytes, pe_offset + 6);
        const auto optional_size = independent_read<std::uint16_t>(bytes, pe_offset + 20);
        const auto optional_offset = pe_offset + 24;
        const auto section_offset = optional_offset + optional_size;

        // Reserve section storage before retaining non-owning views into its strings and records.
        filename = path.filename().string();
        filenames.push_back(filename);
        section_names.reserve(section_count);
        sections.reserve(section_count);

        // Each 40-byte section record starts with an eight-byte name; VirtualSize and RVA follow at offsets 8 and 12.
        for (std::uint16_t index = 0; index < section_count; ++index) {
            const auto offset = section_offset + static_cast<std::size_t>(index) * 40;
            std::array<char, 8> name{};
            std::memcpy(name.data(), bytes.data() + offset, name.size());
            const auto end = std::find(name.begin(), name.end(), '\0');
            section_names.emplace_back(name.begin(), end);
            sections.push_back({section_names.back(), independent_read<std::uint32_t>(bytes, offset + 12),
                                independent_read<std::uint32_t>(bytes, offset + 8)});
        }

        profile = {"Fixture_Image", filenames, independent_read<std::uint32_t>(bytes, optional_offset + 56), sections};
    }
};
} // namespace

TEST_CASE("PE inspection accepts the current architecture and reports section ranges") {
    const auto image = make_image();

    const auto layout = fc::runtime::parse_file_image(image.bytes, "synthetic image");

    REQUIRE(layout);
    REQUIRE(layout->preferred_base == (sizeof(void *) == 8 ? 0x140000000ULL : 0x400000ULL));
    REQUIRE(layout->image_size == image.image_size);
    REQUIRE(layout->sections.size() == 2);
    CHECK(layout->sections[0].name == ".text");
    CHECK(layout->sections[0].rva == image.text_rva);
    CHECK(layout->sections[0].virtual_size == image.text_size);
    CHECK(layout->sections[0].mapped_size == image.text_size);
    CHECK(layout->sections[1].raw_offset == 0x600);
    CHECK(layout->sections[1].raw_size == 0x100);
}

TEST_CASE("recognition preserves zero VirtualSize while using raw size for the mapped extent") {
    auto image = make_image();
    put(image.bytes, kSectionTableOffset + 40 + 8, std::uint32_t{});

    const auto layout = fc::runtime::parse_file_image(image.bytes, "zero VirtualSize image");

    REQUIRE(layout);
    CHECK(layout->sections[1].virtual_size == 0);
    CHECK(layout->sections[1].mapped_size == 0x100);

    constexpr std::array filenames{std::string_view{"fixture.exe"}};
    const std::array sections{fc::runtime::SectionProfile{".text", image.text_rva, image.text_size},
                              fc::runtime::SectionProfile{".data", image.data_rva, 0}};
    const std::array profiles{fc::runtime::ImageProfile{"Zero_Virtual_Size", filenames, image.image_size, sections}};

    const auto match = fc::runtime::recognize_image("fixture.exe", *layout, profiles);

    REQUIRE(match);
    CHECK((*match)->id == "Zero_Virtual_Size");
}

TEST_CASE("PE inspection rejects another architecture, truncated headers and overflowing sections") {
    auto image = make_image();
    put(image.bytes, kFileHeaderOffset, std::uint16_t{sizeof(void *) == 8 ? 0x014c : 0x8664});
    const auto machine = fc::runtime::parse_file_image(image.bytes, "wrong machine");
    REQUIRE_FALSE(machine);
    CHECK(error_contains(machine.error(), "machine"));

    image = make_image();
    const auto truncated = fc::runtime::parse_file_image(std::span{image.bytes}.first(kSectionTableOffset + 60), "truncated image");
    REQUIRE_FALSE(truncated);
    CHECK(error_contains(truncated.error(), "section table"));

    // An RVA plus extent must not wrap through 32-bit arithmetic.
    put(image.bytes, kSectionTableOffset + 12, std::uint32_t{0xfffffff0});
    put(image.bytes, kSectionTableOffset + 8, std::uint32_t{0x40});
    const auto overflow = fc::runtime::parse_file_image(image.bytes, "overflowing section");
    REQUIRE_FALSE(overflow);
    CHECK(error_contains(overflow.error(), "mapped range"));
}

TEST_CASE("recognition accepts image growth and rejects changed identifying facts") {
    auto image = make_image();
    TestCatalog catalog{image};
    auto layout = fc::runtime::parse_file_image(image.bytes, "synthetic image");
    REQUIRE(layout);

    layout->image_size += 0x1000;
    // An appended section may reuse a PE name; recognition still matches the original name, RVA, and VirtualSize.
    layout->sections.push_back({".text", 0x4000, 0x80, 0x80, 0, 0, 0x40000040});

    REQUIRE(fc::runtime::recognize_image("FIXTURE.EXE", *layout, catalog.profiles));
    REQUIRE(fc::runtime::recognize_image("renamed-mod-tools.exe", *layout, catalog.profiles));

    auto changed = *layout;
    changed.sections[0].virtual_size += 1;

    auto result = fc::runtime::recognize_image("fixture.exe", changed, catalog.profiles);

    REQUIRE_FALSE(result);
    CHECK(error_contains(result.error(), "profile catalog has no matching entry"));

    std::array ambiguous{catalog.profiles[0], catalog.profiles[0]};
    ambiguous[1].id = "Other_Profile";
    constexpr std::array other_names{std::string_view{"different.exe"}};
    ambiguous[1].filenames = other_names;

    result = fc::runtime::recognize_image("fixture.exe", *layout, ambiguous);

    REQUIRE_FALSE(result);
    CHECK(error_contains(result.error(), "ambiguous"));
}

TEST_CASE("loaded discovery distinguishes absence, incompatibility, ambiguity, and pins a match", "[.loaded-image]") {
    const std::filesystem::path path{FC_FIXTURE_NO_EXPORT};
    FixtureProfile fixture{path};
    const std::array catalog{fixture.profile};

    // This test runs in its own process because a successful probe pins the fixture until process exit.
    auto probe = fc::runtime::probe_loaded_image(path.filename().string(), catalog);
    REQUIRE(probe);
    REQUIRE_FALSE(*probe);

    const auto module = LoadLibraryW(path.c_str());
    REQUIRE(module != nullptr);

    // Exercise failures for a present module before successful recognition pins it.
    auto incompatible_profile = fixture.profile;
    incompatible_profile.image_size = std::numeric_limits<std::uint32_t>::max();
    const std::array incompatible{incompatible_profile};

    probe = fc::runtime::probe_loaded_image(path.filename().string(), incompatible);

    REQUIRE_FALSE(probe);
    CHECK(error_contains(probe.error(), "profile catalog has no matching entry"));

    std::array ambiguous{fixture.profile, fixture.profile};
    ambiguous[1].id = "Other_Fixture";

    probe = fc::runtime::probe_loaded_image(path.filename().string(), ambiguous);

    REQUIRE_FALSE(probe);
    CHECK(error_contains(probe.error(), "ambiguous"));

    // Compare file and mapped layouts, then release the caller's reference to prove the runtime pin remains.
    const auto bytes = read_file(path);
    const auto file_image = fc::runtime::parse_file_image(bytes, path.string());
    REQUIRE(file_image);

    probe = fc::runtime::probe_loaded_image(path.filename().string(), catalog);
    REQUIRE(probe);
    REQUIRE(*probe);

    const auto recognized = fc::runtime::recognize_image(path.filename().string(), *file_image, catalog);
    REQUIRE(recognized);

    CHECK((*probe)->profile->id == (*recognized)->id);
    CHECK((*probe)->base == reinterpret_cast<std::uintptr_t>(module));
    CHECK((*probe)->image_size == file_image->image_size);
    REQUIRE((*probe)->sections.size() == file_image->sections.size());
    for (std::size_t index = 0; index < (*probe)->sections.size(); ++index) {
        CHECK((*probe)->sections[index].begin == (*probe)->base + file_image->sections[index].rva);
        CHECK((*probe)->sections[index].size == file_image->sections[index].mapped_size);
    }

    REQUIRE(FreeLibrary(module) != 0);
    CHECK(GetModuleHandleW(path.filename().c_str()) != nullptr);
}

TEST_CASE("main module discovery uses the process handle independently of catalog filenames") {
    std::array<wchar_t, 32768> path{};
    REQUIRE(GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size())));
    FixtureProfile fixture{std::filesystem::path{path.data()}};
    constexpr std::array names{std::string_view{"unrelated-catalog-name.exe"}};
    fixture.profile.filenames = names;
    const std::array catalog{fixture.profile};

    const auto observed = fc::runtime::observe_loaded_images({}, catalog);

    REQUIRE(observed.size() == 1);
    CHECK(observed[0].executable);
    REQUIRE(observed[0].outcome);
    REQUIRE(*observed[0].outcome);
    const auto &main = *observed[0].outcome;
    CHECK(main->base == reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)));
    CHECK(main->filename == std::filesystem::path{path.data()}.filename().string());
    CHECK(main->profile == &catalog[0]);
}
