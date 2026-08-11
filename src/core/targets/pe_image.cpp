#include "pe_image.hpp"

#include <Windows.h>

#include <cstring>
#include <limits>
#include <optional>

namespace fusioncutter::targets {
namespace {

template <typename T> [[nodiscard]] std::optional<T> read_value(std::span<const std::byte> image, std::size_t offset) {
    if (offset > image.size() || sizeof(T) > image.size() - offset) {
        return std::nullopt;
    }

    T value{};
    std::memcpy(&value, image.data() + offset, sizeof(value));
    return value;
}

[[nodiscard]] std::optional<std::size_t> checked_add(std::size_t left, std::size_t right) {
    if (left > std::numeric_limits<std::size_t>::max() - right) {
        return std::nullopt;
    }
    return left + right;
}

template <typename T>
[[nodiscard]] std::expected<PeImageFacts, std::string_view>
inspect_optional_header(std::span<const std::byte> mapped_image, const IMAGE_FILE_HEADER& file_header,
                        std::size_t optional_header_offset, Architecture architecture) {
    if (file_header.SizeOfOptionalHeader < sizeof(T)) {
        return std::unexpected("PE optional header is truncated");
    }

    const auto optional_header = read_value<T>(mapped_image, optional_header_offset);
    if (!optional_header.has_value()) {
        return std::unexpected("PE optional header lies outside the mapped image");
    }

    if (optional_header->SizeOfImage == 0 || optional_header->SizeOfImage > mapped_image.size()) {
        return std::unexpected("PE SizeOfImage is outside the mapped image");
    }

    const auto section_table_offset = checked_add(optional_header_offset, file_header.SizeOfOptionalHeader);
    if (!section_table_offset.has_value()) {
        return std::unexpected("PE section table offset overflows");
    }

    const auto section_table_size =
        static_cast<std::size_t>(file_header.NumberOfSections) * sizeof(IMAGE_SECTION_HEADER);
    const auto section_table_end = checked_add(*section_table_offset, section_table_size);
    if (!section_table_end.has_value() || *section_table_end > optional_header->SizeOfHeaders ||
        optional_header->SizeOfHeaders > optional_header->SizeOfImage ||
        optional_header->SizeOfHeaders > mapped_image.size()) {
        return std::unexpected("PE section table is outside SizeOfHeaders");
    }

    PeImageFacts facts{architecture, optional_header->SizeOfImage, file_header.TimeDateStamp, {}};
    facts.sections.reserve(file_header.NumberOfSections);

    for (std::size_t index = 0; index < file_header.NumberOfSections; ++index) {
        const auto section_offset = checked_add(*section_table_offset, index * sizeof(IMAGE_SECTION_HEADER));
        if (!section_offset.has_value()) {
            return std::unexpected("PE section offset overflows");
        }

        const auto section = read_value<IMAGE_SECTION_HEADER>(mapped_image, *section_offset);
        if (!section.has_value()) {
            return std::unexpected("PE section header lies outside the mapped image");
        }

        const auto virtual_address = static_cast<std::size_t>(section->VirtualAddress);
        const auto virtual_size = static_cast<std::size_t>(section->Misc.VirtualSize);
        if (virtual_address > facts.size_of_image || virtual_size > facts.size_of_image - virtual_address) {
            return std::unexpected("PE section lies outside SizeOfImage");
        }

        PeSectionFacts section_facts{};
        std::memcpy(section_facts.name.data(), section->Name, section_facts.name.size());
        section_facts.virtual_address = section->VirtualAddress;
        section_facts.virtual_size = section->Misc.VirtualSize;
        facts.sections.push_back(section_facts);
    }

    return facts;
}

} // namespace

std::expected<PeImageFacts, std::string_view> inspect_mapped_pe(std::span<const std::byte> mapped_image) {
    const auto dos_header = read_value<IMAGE_DOS_HEADER>(mapped_image, 0);
    if (!dos_header.has_value() || dos_header->e_magic != IMAGE_DOS_SIGNATURE || dos_header->e_lfanew < 0) {
        return std::unexpected("mapped image has no valid DOS header");
    }

    const auto nt_offset = static_cast<std::size_t>(dos_header->e_lfanew);
    const auto signature = read_value<DWORD>(mapped_image, nt_offset);
    if (!signature.has_value() || *signature != IMAGE_NT_SIGNATURE) {
        return std::unexpected("mapped image has no valid PE signature");
    }

    const auto file_header_offset = checked_add(nt_offset, sizeof(DWORD));
    if (!file_header_offset.has_value()) {
        return std::unexpected("PE file header offset overflows");
    }

    const auto file_header = read_value<IMAGE_FILE_HEADER>(mapped_image, *file_header_offset);
    if (!file_header.has_value() || file_header->NumberOfSections == 0) {
        return std::unexpected("PE file header is missing or has no sections");
    }

    const auto optional_header_offset = checked_add(*file_header_offset, sizeof(IMAGE_FILE_HEADER));
    if (!optional_header_offset.has_value()) {
        return std::unexpected("PE optional header offset overflows");
    }

    const auto magic = read_value<WORD>(mapped_image, *optional_header_offset);
    if (!magic.has_value()) {
        return std::unexpected("PE optional header magic is missing");
    }

    if (file_header->Machine == IMAGE_FILE_MACHINE_I386 && *magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
        return inspect_optional_header<IMAGE_OPTIONAL_HEADER32>(mapped_image, *file_header, *optional_header_offset,
                                                                Architecture::X86);
    }

    if (file_header->Machine == IMAGE_FILE_MACHINE_AMD64 && *magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        return inspect_optional_header<IMAGE_OPTIONAL_HEADER64>(mapped_image, *file_header, *optional_header_offset,
                                                                Architecture::X64);
    }

    return std::unexpected("PE machine and optional-header format are unsupported or inconsistent");
}

} // namespace fusioncutter::targets
