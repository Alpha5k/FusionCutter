#include "targets/target_profiles.hpp"

#include <FusionCutter/Testing.hpp>

#include <catch2/catch_test_macros.hpp>

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <ranges>
#include <string>
#include <vector>

namespace {

// The fixture uses reviewed profile declarations only to synthesize bytes laid out as the loader would provide them;
// Scenario sees solely the public profile ID and borrowed byte span that a plugin author supplies.
[[nodiscard]] std::vector<std::byte> mapped_image(const fc::targets::ImageProfile& profile) {
    // Reproduce the mapped DOS, COFF, and optional-header facts used by production fingerprint recognition.
    std::vector<std::byte> image(profile.size_of_image);
    IMAGE_DOS_HEADER dos{.e_magic = IMAGE_DOS_SIGNATURE, .e_lfanew = 0x80};
    std::memcpy(image.data(), &dos, sizeof(dos));
    const DWORD signature = IMAGE_NT_SIGNATURE;
    std::memcpy(image.data() + dos.e_lfanew, &signature, sizeof(signature));

    IMAGE_FILE_HEADER file{};
    file.Machine = profile.architecture == FC_ARCH_X86 ? IMAGE_FILE_MACHINE_I386 : IMAGE_FILE_MACHINE_AMD64;
    file.NumberOfSections = static_cast<WORD>(profile.sections.size());
    file.TimeDateStamp = profile.timestamp.value_or(0);
    file.SizeOfOptionalHeader =
        profile.architecture == FC_ARCH_X86 ? sizeof(IMAGE_OPTIONAL_HEADER32) : sizeof(IMAGE_OPTIONAL_HEADER64);
    const auto file_offset = static_cast<std::size_t>(dos.e_lfanew) + sizeof(DWORD);
    std::memcpy(image.data() + file_offset, &file, sizeof(file));
    const auto optional_offset = file_offset + sizeof(file);
    if (profile.architecture == FC_ARCH_X86) {
        IMAGE_OPTIONAL_HEADER32 optional{};
        optional.Magic = IMAGE_NT_OPTIONAL_HDR32_MAGIC;
        optional.SizeOfImage = profile.size_of_image;
        optional.SizeOfHeaders = 0x400;
        std::memcpy(image.data() + optional_offset, &optional, sizeof(optional));
    } else {
        IMAGE_OPTIONAL_HEADER64 optional{};
        optional.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
        optional.SizeOfImage = profile.size_of_image;
        optional.SizeOfHeaders = 0x400;
        std::memcpy(image.data() + optional_offset, &optional, sizeof(optional));
    }

    // Section permissions derive the production read, write, and execute policy exercised by Scenario validation.
    const auto section_offset = optional_offset + file.SizeOfOptionalHeader;
    for (std::size_t index = 0; index < profile.sections.size(); ++index) {
        IMAGE_SECTION_HEADER section{};
        std::memcpy(section.Name, profile.sections[index].name.data(), profile.sections[index].name.size());
        section.VirtualAddress = profile.sections[index].virtual_address;
        section.Misc.VirtualSize = profile.sections[index].virtual_size;
        section.Characteristics = IMAGE_SCN_MEM_READ;
        if (profile.sections[index].name == ".text") {
            section.Characteristics |= IMAGE_SCN_MEM_EXECUTE;
        }
        if (profile.sections[index].name == ".data") {
            section.Characteristics |= IMAGE_SCN_MEM_WRITE;
        }
        std::memcpy(image.data() + section_offset + index * sizeof(section), &section, sizeof(section));
    }
    return image;
}

// Select one complete production startup tuple for the current test architecture.
[[nodiscard]] std::vector<const fc::targets::ImageProfile*> startup_profiles() {
    std::vector<const fc::targets::ImageProfile*> result;
    for (const auto& profile : fc::targets::known_image_profiles()) {
        if (profile.architecture != (sizeof(void*) == 4 ? FC_ARCH_X86 : FC_ARCH_X64) || profile.late) {
            continue;
        }
        if constexpr (sizeof(void*) == 4) {
            if (profile.layout == FC_LAYOUT_STEAM_RETAIL) {
                result.push_back(&profile);
                break;
            }
        } else if (profile.layout == FC_LAYOUT_CLASSIC_COLLECTION) {
            result.push_back(&profile);
        }
    }
    return result;
}

} // namespace

TEST_CASE("the public testing scenario owns results and leaves borrowed images unchanged") {
    const auto profiles = startup_profiles();
    REQUIRE_FALSE(profiles.empty());
    const auto layout = static_cast<fc::TargetLayout>(profiles.front()->layout);
    fc::test::Scenario scenario{layout, fc::HostRole::Client};
    std::vector<std::vector<std::byte>> images;
    images.reserve(profiles.size());
    for (const auto* profile : profiles) {
        images.push_back(mapped_image(*profile));
        // Synthetic identity is explicit: changed fingerprint metadata belongs to verifier, not target recognition.
        IMAGE_DOS_HEADER dos{};
        std::memcpy(&dos, images.back().data(), sizeof(dos));
        const auto file_offset = static_cast<std::size_t>(dos.e_lfanew) + sizeof(DWORD);
        IMAGE_FILE_HEADER file{};
        std::memcpy(&file, images.back().data() + file_offset, sizeof(file));
        ++file.TimeDateStamp;
        std::memcpy(images.back().data() + file_offset, &file, sizeof(file));
        scenario.add_image(static_cast<fc::TargetImage>(profile->image), std::string{profile->id}, images.back());
    }
    const auto before = images;

    const auto result = scenario.validate();
    REQUIRE(result);
    const auto* core = result->find_plugin("core");
    REQUIRE(core != nullptr);
    CHECK(core->admitted);
    CHECK_FALSE(core->path);
    CHECK(result->patches().empty());
    REQUIRE(images.size() == before.size());
    for (std::size_t index = 0; index < images.size(); ++index) {
        REQUIRE(images[index].size() == before[index].size());
        CHECK(std::memcmp(images[index].data(), before[index].data(), images[index].size()) == 0);
    }
}

TEST_CASE("the public testing scenario rejects duplicate image and normalized plugin inputs at the top level") {
    const auto profiles = startup_profiles();
    REQUIRE_FALSE(profiles.empty());
    auto image = mapped_image(*profiles.front());
    fc::test::Scenario duplicate_image{static_cast<fc::TargetLayout>(profiles.front()->layout), fc::HostRole::Client};
    duplicate_image.add_image(static_cast<fc::TargetImage>(profiles.front()->image), std::string{profiles.front()->id},
                              image);
    duplicate_image.add_image(static_cast<fc::TargetImage>(profiles.front()->image), std::string{profiles.front()->id},
                              image);
    CHECK_FALSE(duplicate_image.validate());

    fc::test::Scenario duplicate_plugin{static_cast<fc::TargetLayout>(profiles.front()->layout), fc::HostRole::Client};
    duplicate_plugin.add_plugin("same.dll");
    duplicate_plugin.add_plugin(".\\same.dll");
    const auto result = duplicate_plugin.validate();
    REQUIRE_FALSE(result);
    CHECK(result.error().operation == "Normalize plugin inputs");
}
