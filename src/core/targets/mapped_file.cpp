#include "mapped_file.hpp"

#include <Windows.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <limits>
#include <optional>
#include <span>
#include <utility>

namespace fc::targets {
namespace {

// These bounded readers copy potentially unaligned PE fields without ever forming an unchecked native pointer.
template <class Value>
[[nodiscard]] std::optional<Value> read_value(std::span<const std::byte> bytes, std::size_t offset) noexcept {
    if (offset > bytes.size() || sizeof(Value) > bytes.size() - offset) {
        return std::nullopt;
    }
    Value result{};
    std::memcpy(&result, bytes.data() + offset, sizeof(result));
    return result;
}

[[nodiscard]] bool contains(std::span<const std::byte> bytes, std::size_t offset, std::size_t size) noexcept {
    return offset <= bytes.size() && size <= bytes.size() - offset;
}

// FileFacts retains the validated dimensions from the file layout needed to reproduce the loader's mapped RVA layout.
struct FileFacts {
    std::uintptr_t preferred_base{};
    std::uint32_t image_size{};
    std::uint32_t header_size{};
    std::size_t section_table{};
    std::uint16_t section_count{};
    std::uint32_t relocation_rva{};
    std::uint32_t relocation_size{};
};

// Validates the active optional header and retains only the facts needed to reproduce a loaded image.
template <class OptionalHeader>
[[nodiscard]] std::expected<FileFacts, std::string> read_optional_header(std::span<const std::byte> file,
                                                                         const IMAGE_FILE_HEADER& file_header,
                                                                         std::size_t optional_offset) {
    if (file_header.SizeOfOptionalHeader < sizeof(OptionalHeader)) {
        return std::unexpected("The PE optional header is truncated");
    }
    const auto header = read_value<OptionalHeader>(file, optional_offset);
    if (!header || header->SizeOfImage == 0 || header->SizeOfHeaders == 0 ||
        header->ImageBase > std::numeric_limits<std::uintptr_t>::max()) {
        return std::unexpected("The PE optional header has invalid image dimensions");
    }
    if (optional_offset > std::numeric_limits<std::size_t>::max() - file_header.SizeOfOptionalHeader) {
        return std::unexpected("The PE section table offset overflows");
    }
    IMAGE_DATA_DIRECTORY relocations{};
    if (header->NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_BASERELOC) {
        relocations = header->DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
    }
    return FileFacts{static_cast<std::uintptr_t>(header->ImageBase),
                     header->SizeOfImage,
                     header->SizeOfHeaders,
                     optional_offset + file_header.SizeOfOptionalHeader,
                     file_header.NumberOfSections,
                     relocations.VirtualAddress,
                     relocations.Size};
}

// Reads one immutable file snapshot so inspection and section copying cannot observe different source bytes.
[[nodiscard]] std::expected<std::vector<std::byte>, std::string> read_file(const std::filesystem::path& path) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size == 0 || size > std::numeric_limits<std::size_t>::max() ||
        size > static_cast<std::uintmax_t>(std::numeric_limits<std::streamsize>::max())) {
        return std::unexpected("The image file has an invalid or unreadable size");
    }
    std::ifstream input{path, std::ios::binary};
    if (!input) {
        return std::unexpected("The image file could not be opened for reading");
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    if (!input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
        return std::unexpected("The complete image file could not be read");
    }
    return bytes;
}

[[nodiscard]] std::expected<FileFacts, std::string> inspect_file(std::span<const std::byte> file) {
    // Establish bounded DOS and PE header offsets before interpreting architecture-dependent fields.
    const auto dos = read_value<IMAGE_DOS_HEADER>(file, 0);
    if (!dos || dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0) {
        return std::unexpected("The image file has no valid DOS header");
    }
    const auto nt_offset = static_cast<std::size_t>(dos->e_lfanew);
    const auto signature = read_value<DWORD>(file, nt_offset);
    if (!signature || *signature != IMAGE_NT_SIGNATURE ||
        nt_offset > std::numeric_limits<std::size_t>::max() - sizeof(DWORD)) {
        return std::unexpected("The image file has no valid PE signature");
    }
    const auto file_header_offset = nt_offset + sizeof(DWORD);
    const auto file_header = read_value<IMAGE_FILE_HEADER>(file, file_header_offset);
    if (!file_header || file_header->NumberOfSections == 0 ||
        file_header_offset > std::numeric_limits<std::size_t>::max() - sizeof(IMAGE_FILE_HEADER)) {
        return std::unexpected("The PE file header is missing or invalid");
    }
    const auto optional_offset = file_header_offset + sizeof(IMAGE_FILE_HEADER);
    const auto magic = read_value<WORD>(file, optional_offset);
    if (!magic) {
        return std::unexpected("The PE optional header is missing");
    }
    if constexpr (sizeof(void*) == 4) {
        if (file_header->Machine != IMAGE_FILE_MACHINE_I386 || *magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
            return std::unexpected("The PE architecture does not match this x86 host");
        }
        return read_optional_header<IMAGE_OPTIONAL_HEADER32>(file, *file_header, optional_offset);
    } else {
        if (file_header->Machine != IMAGE_FILE_MACHINE_AMD64 || *magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
            return std::unexpected("The PE architecture does not match this x64 host");
        }
        return read_optional_header<IMAGE_OPTIONAL_HEADER64>(file, *file_header, optional_offset);
    }
}

[[nodiscard]] std::expected<void, std::string> map_sections(std::span<const std::byte> file, const FileFacts& facts,
                                                            std::span<std::byte> image) {
    // Reproduce the loaded header and RVA layout in private storage while retaining zero-filled virtual tails.
    if (!contains(file, 0, facts.header_size) || facts.header_size > image.size()) {
        return std::unexpected("The PE headers lie outside the file or mapped image");
    }
    std::memcpy(image.data(), file.data(), facts.header_size);
    for (std::size_t index = 0; index < facts.section_count; ++index) {
        if (index > (std::numeric_limits<std::size_t>::max() - facts.section_table) / sizeof(IMAGE_SECTION_HEADER)) {
            return std::unexpected("The PE section header offset overflows");
        }
        const auto section =
            read_value<IMAGE_SECTION_HEADER>(file, facts.section_table + index * sizeof(IMAGE_SECTION_HEADER));
        if (!section) {
            return std::unexpected("A PE section header lies outside the file");
        }
        const auto virtual_address = static_cast<std::size_t>(section->VirtualAddress);
        const auto virtual_size = static_cast<std::size_t>(section->Misc.VirtualSize);
        const auto raw_offset = static_cast<std::size_t>(section->PointerToRawData);
        const auto raw_size = static_cast<std::size_t>(section->SizeOfRawData);
        if (!contains(image, virtual_address, std::max(virtual_size, raw_size)) ||
            !contains(file, raw_offset, raw_size)) {
            return std::unexpected("A PE section lies outside the file or mapped image");
        }
        if (raw_size != 0) {
            std::memcpy(image.data() + virtual_address, file.data() + raw_offset, raw_size);
        }
    }
    return {};
}

[[nodiscard]] std::expected<void, std::string> relocate(std::span<std::byte> image, const FileFacts& facts) {
    // Relocations target the private vector so absolute evidence sees the values loading would produce.
    const auto actual_base = reinterpret_cast<std::uintptr_t>(image.data());
    if (actual_base == facts.preferred_base || facts.relocation_rva == 0 || facts.relocation_size == 0) {
        return {};
    }
    if (!contains(image, facts.relocation_rva, facts.relocation_size)) {
        return std::unexpected("The PE relocation directory lies outside the mapped image");
    }
    const auto directory_end = static_cast<std::size_t>(facts.relocation_rva) + facts.relocation_size;
    auto block_offset = static_cast<std::size_t>(facts.relocation_rva);
    // Validate each complete block before visiting entries so malformed block sizes cannot escape the directory.
    while (block_offset < directory_end) {
        const auto block = read_value<IMAGE_BASE_RELOCATION>(image, block_offset);
        if (!block || block->SizeOfBlock < sizeof(IMAGE_BASE_RELOCATION) ||
            block->SizeOfBlock > directory_end - block_offset) {
            return std::unexpected("A PE relocation block is malformed");
        }
        const auto entry_bytes = block->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION);
        if (entry_bytes % sizeof(std::uint16_t) != 0) {
            return std::unexpected("The PE relocation entries are misaligned");
        }
        const auto entries = entry_bytes / sizeof(std::uint16_t);
        // Apply only the pointer-width relocation form supported by the active verifier architecture.
        for (std::size_t index = 0; index < entries; ++index) {
            const auto entry = read_value<std::uint16_t>(image, block_offset + sizeof(IMAGE_BASE_RELOCATION) +
                                                                    index * sizeof(std::uint16_t));
            if (!entry) {
                return std::unexpected("A PE relocation entry lies outside the mapped image");
            }
            const auto type = static_cast<std::uint16_t>(*entry >> 12U);
            if (type == IMAGE_REL_BASED_ABSOLUTE) {
                continue;
            }
            constexpr auto expected = sizeof(void*) == 4 ? IMAGE_REL_BASED_HIGHLOW : IMAGE_REL_BASED_DIR64;
            if (type != expected) {
                return std::unexpected("The PE relocation type is unsupported by this host");
            }
            const auto page_offset = static_cast<std::uint32_t>(*entry & 0x0fffU);
            if (block->VirtualAddress > std::numeric_limits<std::uint32_t>::max() - page_offset) {
                return std::unexpected("A PE relocation RVA overflows");
            }
            const auto target = block->VirtualAddress + page_offset;
            if (!contains(image, target, sizeof(std::uintptr_t))) {
                return std::unexpected("A PE relocation target lies outside the mapped image");
            }
            std::uintptr_t value{};
            std::memcpy(&value, image.data() + target, sizeof(value));
            value = actual_base >= facts.preferred_base ? value + (actual_base - facts.preferred_base)
                                                        : value - (facts.preferred_base - actual_base);
            std::memcpy(image.data() + target, &value, sizeof(value));
        }
        block_offset += block->SizeOfBlock;
    }
    return {};
}

} // namespace

std::expected<std::vector<std::byte>, std::string> map_pe_file(const std::filesystem::path& path) {
    // File inspection precedes allocation so malformed dimensions cannot control the mapped image owner.
    auto file = read_file(path);
    if (!file) {
        return std::unexpected(std::move(file.error()));
    }
    auto facts = inspect_file(*file);
    if (!facts) {
        return std::unexpected(std::move(facts.error()));
    }
    std::vector<std::byte> image(facts->image_size);
    // Section copying and relocation operate only on the private vector; the source stream was opened read-only.
    if (auto mapped = map_sections(*file, *facts, image); !mapped) {
        return std::unexpected(std::move(mapped.error()));
    }
    if (auto relocated = relocate(image, *facts); !relocated) {
        return std::unexpected(std::move(relocated.error()));
    }
    return image;
}

} // namespace fc::targets
