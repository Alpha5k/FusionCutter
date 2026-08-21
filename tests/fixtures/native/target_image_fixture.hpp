#pragma once

#include "targets/target_profiles.hpp"

#include <FusionCutter/SDK.hpp>

#include <Windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace fc::fixtures {

// One payload is copied into a reviewed virtual location in both mapped and raw PE fixture forms.
struct ImagePayload {
    fc::Rva rva;
    std::span<const std::byte> bytes;
};

namespace detail {

// Returns the reviewed section that contains an entire payload, not merely its first byte.
[[nodiscard]] inline const targets::SectionProfile* section_for(const targets::ImageProfile& profile, fc::Rva rva,
                                                                std::size_t byte_size) noexcept {
    if (byte_size > std::numeric_limits<std::uint64_t>::max() - rva.value) {
        return nullptr;
    }
    const auto payload_end = static_cast<std::uint64_t>(rva.value) + byte_size;
    for (const auto& section : profile.sections) {
        const auto section_end = static_cast<std::uint64_t>(section.virtual_address) + section.virtual_size;
        if (rva.value >= section.virtual_address && rva.value < section_end && payload_end <= section_end) {
            return &section;
        }
    }
    return nullptr;
}

// Writes the common PE headers used by loaded-layout Scenario bytes and compact verifier files.
inline void write_headers(std::span<std::byte> bytes, const targets::ImageProfile& profile,
                          std::span<const IMAGE_SECTION_HEADER> sections, std::uint32_t size_of_headers) {
    if (bytes.size() < size_of_headers) {
        throw std::invalid_argument{"The target image fixture is smaller than its PE headers"};
    }

    // Establish the DOS and COFF headers before selecting the architecture-specific optional header.
    IMAGE_DOS_HEADER dos{.e_magic = IMAGE_DOS_SIGNATURE, .e_lfanew = 0x80};
    std::memcpy(bytes.data(), &dos, sizeof(dos));
    const DWORD signature = IMAGE_NT_SIGNATURE;
    std::memcpy(bytes.data() + dos.e_lfanew, &signature, sizeof(signature));

    IMAGE_FILE_HEADER file{};
    file.Machine = profile.architecture == FC_ARCH_X86 ? IMAGE_FILE_MACHINE_I386 : IMAGE_FILE_MACHINE_AMD64;
    file.NumberOfSections = static_cast<WORD>(sections.size());
    file.TimeDateStamp = profile.timestamp.value_or(0);
    file.SizeOfOptionalHeader =
        profile.architecture == FC_ARCH_X86 ? sizeof(IMAGE_OPTIONAL_HEADER32) : sizeof(IMAGE_OPTIONAL_HEADER64);
    const auto file_offset = static_cast<std::size_t>(dos.e_lfanew) + sizeof(DWORD);
    std::memcpy(bytes.data() + file_offset, &file, sizeof(file));

    const auto optional_offset = file_offset + sizeof(file);
    // Scenario and verifier inputs must expose the exact architecture and reviewed image extent.
    if (profile.architecture == FC_ARCH_X86) {
        IMAGE_OPTIONAL_HEADER32 optional{};
        optional.Magic = IMAGE_NT_OPTIONAL_HDR32_MAGIC;
        optional.SizeOfImage = profile.size_of_image;
        optional.SizeOfHeaders = size_of_headers;
        optional.SectionAlignment = 0x1000;
        optional.FileAlignment = 0x200;
        std::memcpy(bytes.data() + optional_offset, &optional, sizeof(optional));
    } else {
        IMAGE_OPTIONAL_HEADER64 optional{};
        optional.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
        optional.SizeOfImage = profile.size_of_image;
        optional.SizeOfHeaders = size_of_headers;
        optional.SectionAlignment = 0x1000;
        optional.FileAlignment = 0x200;
        std::memcpy(bytes.data() + optional_offset, &optional, sizeof(optional));
    }

    const auto section_offset = optional_offset + file.SizeOfOptionalHeader;
    if (section_offset > size_of_headers || sections.size_bytes() > size_of_headers - section_offset) {
        throw std::invalid_argument{"The target image section table exceeds its PE headers"};
    }
    std::memcpy(bytes.data() + section_offset, sections.data(), sections.size_bytes());
}

// Supplies the access policy expected by production image validation for each reviewed section kind.
[[nodiscard]] inline DWORD section_characteristics(std::string_view name) noexcept {
    if (name == ".text") {
        return IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ;
    }
    if (name == ".data") {
        return IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE;
    }
    return IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ;
}

} // namespace detail

// Builds loaded-layout bytes for Scenario while preserving the selected profile's exact PE and access facts.
[[nodiscard]] inline std::vector<std::byte> mapped_target_image(const targets::ImageProfile& profile,
                                                                std::span<const ImagePayload> payloads = {}) {
    constexpr std::uint32_t kHeaderSize = 0x400;
    std::vector<std::byte> result(profile.size_of_image);

    // Translate reviewed virtual sections directly because Scenario consumes an already loaded image layout.
    std::vector<IMAGE_SECTION_HEADER> sections;
    sections.reserve(profile.sections.size());
    for (const auto& source : profile.sections) {
        IMAGE_SECTION_HEADER section{};
        std::memcpy(section.Name, source.name.data(), std::min(source.name.size(), sizeof(section.Name)));
        section.VirtualAddress = source.virtual_address;
        section.Misc.VirtualSize = source.virtual_size;
        section.Characteristics = detail::section_characteristics(source.name);
        sections.push_back(section);
    }
    detail::write_headers(result, profile, sections, kHeaderSize);

    // Fixture payloads must fit wholly inside one reviewed section before they become validation evidence.
    for (const auto& payload : payloads) {
        if (payload.rva.value > result.size() || payload.bytes.size() > result.size() - payload.rva.value ||
            detail::section_for(profile, payload.rva, payload.bytes.size()) == nullptr) {
            throw std::invalid_argument{"A payload lies outside the reviewed target image"};
        }
        std::memcpy(result.data() + payload.rva.value, payload.bytes.data(), payload.bytes.size());
    }
    return result;
}

// Builds a compact on-disk PE whose mapped image has the same reviewed facts and payloads used by Scenario.
[[nodiscard]] inline std::vector<std::byte> raw_target_image(const targets::ImageProfile& profile,
                                                             std::span<const ImagePayload> payloads = {}) {
    constexpr std::uint32_t kHeaderSize = 0x400;
    constexpr std::uint32_t kFileAlignment = 0x200;
    std::vector<IMAGE_SECTION_HEADER> sections;
    sections.reserve(profile.sections.size());
    std::uint32_t next_raw = kHeaderSize;

    // Materialize only the raw section bytes needed to carry supplied payloads into the mapped verifier image.
    for (const auto& source : profile.sections) {
        std::uint32_t required{};
        for (const auto& payload : payloads) {
            if (const auto* owner = detail::section_for(profile, payload.rva, payload.bytes.size()); owner == &source) {
                required = std::max(required, payload.rva.value - source.virtual_address +
                                                  static_cast<std::uint32_t>(payload.bytes.size()));
            }
        }
        const auto raw_size = required == 0 ? 0U : (required + kFileAlignment - 1U) & ~(kFileAlignment - 1U);
        IMAGE_SECTION_HEADER section{};
        std::memcpy(section.Name, source.name.data(), std::min(source.name.size(), sizeof(section.Name)));
        section.VirtualAddress = source.virtual_address;
        section.Misc.VirtualSize = source.virtual_size;
        section.PointerToRawData = raw_size == 0 ? 0 : next_raw;
        section.SizeOfRawData = raw_size;
        section.Characteristics = detail::section_characteristics(source.name);
        sections.push_back(section);
        next_raw += raw_size;
    }

    std::vector<std::byte> result(next_raw);
    detail::write_headers(result, profile, sections, kHeaderSize);

    // Convert each virtual payload RVA back to its owning section's compact file offset.
    for (const auto& payload : payloads) {
        const auto* source = detail::section_for(profile, payload.rva, payload.bytes.size());
        if (source == nullptr) {
            throw std::invalid_argument{"A payload lies outside the reviewed target image"};
        }
        const auto index = static_cast<std::size_t>(source - profile.sections.data());
        const auto file_offset = sections[index].PointerToRawData + payload.rva.value - source->virtual_address;
        if (file_offset > result.size() || payload.bytes.size() > result.size() - file_offset) {
            throw std::invalid_argument{"A payload exceeds its raw target image section"};
        }
        std::memcpy(result.data() + file_offset, payload.bytes.data(), payload.bytes.size());
    }
    return result;
}

} // namespace fc::fixtures
