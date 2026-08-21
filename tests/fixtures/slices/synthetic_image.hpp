#pragma once

#include <FusionCutter/SDK.hpp>

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace fc::fixtures {

// Public Scenario fixtures describe only the access policy and payload needed by the submitted public patch plan.
struct SyntheticSection {
    std::string_view name;
    fc::Rva rva;
    std::uint32_t byte_size;
    DWORD characteristics;
};

// One payload supplies bytes at a virtual address owned by a declared synthetic section.
struct SyntheticPayload {
    fc::Rva rva;
    std::span<const std::byte> bytes;
};

// Builds valid loaded PE structure while leaving identity selection to Scenario's explicit reviewed profile input.
[[nodiscard]] inline std::vector<std::byte> synthetic_image(fc::Architecture architecture, std::uint32_t image_size,
                                                            std::span<const SyntheticSection> source_sections,
                                                            std::span<const SyntheticPayload> payloads = {}) {
    constexpr std::uint32_t kHeaderSize = 0x400;
    if (image_size < kHeaderSize) {
        throw std::invalid_argument{"The synthetic image is smaller than its PE headers"};
    }
    std::vector<std::byte> result(image_size);

    // Write the common loaded image headers and the selected architecture's optional header.
    IMAGE_DOS_HEADER dos{.e_magic = IMAGE_DOS_SIGNATURE, .e_lfanew = 0x80};
    std::memcpy(result.data(), &dos, sizeof(dos));
    const DWORD signature = IMAGE_NT_SIGNATURE;
    std::memcpy(result.data() + dos.e_lfanew, &signature, sizeof(signature));

    IMAGE_FILE_HEADER file{};
    file.Machine = architecture == fc::Architecture::X86 ? IMAGE_FILE_MACHINE_I386 : IMAGE_FILE_MACHINE_AMD64;
    file.NumberOfSections = static_cast<WORD>(source_sections.size());
    file.SizeOfOptionalHeader =
        architecture == fc::Architecture::X86 ? sizeof(IMAGE_OPTIONAL_HEADER32) : sizeof(IMAGE_OPTIONAL_HEADER64);
    const auto file_offset = static_cast<std::size_t>(dos.e_lfanew) + sizeof(DWORD);
    std::memcpy(result.data() + file_offset, &file, sizeof(file));

    const auto optional_offset = file_offset + sizeof(file);
    if (architecture == fc::Architecture::X86) {
        IMAGE_OPTIONAL_HEADER32 optional{};
        optional.Magic = IMAGE_NT_OPTIONAL_HDR32_MAGIC;
        optional.SizeOfImage = image_size;
        optional.SizeOfHeaders = kHeaderSize;
        std::memcpy(result.data() + optional_offset, &optional, sizeof(optional));
    } else {
        IMAGE_OPTIONAL_HEADER64 optional{};
        optional.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
        optional.SizeOfImage = image_size;
        optional.SizeOfHeaders = kHeaderSize;
        std::memcpy(result.data() + optional_offset, &optional, sizeof(optional));
    }

    const auto section_offset = optional_offset + file.SizeOfOptionalHeader;
    if (section_offset > kHeaderSize ||
        source_sections.size() > (kHeaderSize - section_offset) / sizeof(IMAGE_SECTION_HEADER)) {
        throw std::invalid_argument{"The synthetic image section table exceeds its headers"};
    }
    // Section declarations define the exact access policy later enforced by production validation.
    for (std::size_t index = 0; index < source_sections.size(); ++index) {
        const auto& source = source_sections[index];
        if (source.rva.value > image_size || source.byte_size > image_size - source.rva.value) {
            throw std::invalid_argument{"A synthetic image section exceeds SizeOfImage"};
        }
        IMAGE_SECTION_HEADER section{};
        std::memcpy(section.Name, source.name.data(), std::min(source.name.size(), sizeof(section.Name)));
        section.VirtualAddress = source.rva.value;
        section.Misc.VirtualSize = source.byte_size;
        section.Characteristics = source.characteristics;
        std::memcpy(result.data() + section_offset + index * sizeof(section), &section, sizeof(section));
    }

    // Reject bytes that are in the image allocation but outside every declared PE section.
    for (const auto& payload : payloads) {
        if (payload.bytes.size() > std::numeric_limits<std::uint64_t>::max() - payload.rva.value) {
            throw std::invalid_argument{"A synthetic image payload range overflows"};
        }
        const auto end = static_cast<std::uint64_t>(payload.rva.value) + payload.bytes.size();
        const auto owned = std::ranges::any_of(source_sections, [&](const auto& section) {
            return payload.rva.value >= section.rva.value &&
                   end <= static_cast<std::uint64_t>(section.rva.value) + section.byte_size;
        });
        if (!owned || end > result.size()) {
            throw std::invalid_argument{"A synthetic image payload lies outside its sections"};
        }
        std::memcpy(result.data() + payload.rva.value, payload.bytes.data(), payload.bytes.size());
    }
    return result;
}

// The common form keeps scenarios with one writable data section concise.
[[nodiscard]] inline std::vector<std::byte> synthetic_image(fc::Architecture architecture, std::uint32_t image_size,
                                                            fc::Rva writable_rva,
                                                            std::span<const std::byte> payload = {}) {
    const auto section_rva = writable_rva.value & ~std::uint32_t{0xfff};
    const auto offset = writable_rva.value - section_rva;
    if (payload.size() > std::numeric_limits<std::uint32_t>::max() - offset) {
        throw std::invalid_argument{"A synthetic image payload is too large"};
    }
    const auto required = offset + static_cast<std::uint32_t>(payload.size());
    const std::array sections{
        SyntheticSection{".data",
                         {section_rva},
                         std::max<std::uint32_t>(0x1000, required),
                         IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE}};
    const std::array payloads{SyntheticPayload{writable_rva, payload}};
    return synthetic_image(architecture, image_size, sections,
                           payload.empty() ? std::span<const SyntheticPayload>{} : std::span{payloads});
}

} // namespace fc::fixtures
