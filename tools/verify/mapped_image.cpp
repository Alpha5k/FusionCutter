#include "mapped_image.hpp"

#include <Windows.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace fusioncutter::verify {
namespace {

template <typename T> [[nodiscard]] std::optional<T> read_value(std::span<const std::byte> input, std::size_t offset) {
    if (offset > input.size() || sizeof(T) > input.size() - offset) {
        return std::nullopt;
    }

    T value{};
    std::memcpy(&value, input.data() + offset, sizeof(value));
    return value;
}

[[nodiscard]] bool contains(std::span<const std::byte> input, std::size_t offset, std::size_t size) noexcept {
    return offset <= input.size() && size <= input.size() - offset;
}

struct ImageFacts {
    std::uintptr_t preferred_base;
    std::uint32_t size;
    std::uint32_t header_size;
    std::size_t section_table;
    std::uint16_t section_count;
};

template <typename OptionalHeader>
[[nodiscard]] std::expected<ImageFacts, std::string> read_image_facts(std::span<const std::byte> file,
                                                                      const IMAGE_FILE_HEADER& file_header,
                                                                      std::size_t optional_header_offset) {
    if (file_header.SizeOfOptionalHeader < sizeof(OptionalHeader)) {
        return std::unexpected("PE optional header is truncated");
    }
    const auto header = read_value<OptionalHeader>(file, optional_header_offset);
    if (!header.has_value() || header->SizeOfImage == 0 || header->SizeOfHeaders == 0) {
        return std::unexpected("PE optional header has invalid image dimensions");
    }
    if (header->ImageBase > std::numeric_limits<std::uintptr_t>::max()) {
        return std::unexpected("PE image base cannot be represented by this verifier");
    }
    if (optional_header_offset > std::numeric_limits<std::size_t>::max() - file_header.SizeOfOptionalHeader) {
        return std::unexpected("PE section table offset overflows");
    }

    return ImageFacts{
        static_cast<std::uintptr_t>(header->ImageBase),
        header->SizeOfImage,
        header->SizeOfHeaders,
        optional_header_offset + file_header.SizeOfOptionalHeader,
        file_header.NumberOfSections,
    };
}

[[nodiscard]] std::expected<std::vector<std::byte>, std::string> read_file(const std::filesystem::path& path) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error) {
        return std::unexpected("could not determine the file size");
    }
    if (size == 0 || size > std::numeric_limits<std::size_t>::max() ||
        size > static_cast<std::uintmax_t>(std::numeric_limits<std::streamsize>::max())) {
        return std::unexpected("file size cannot be mapped by this verifier");
    }

    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return std::unexpected("could not open the file for reading");
    }

    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    if (!input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
        return std::unexpected("could not read the complete file");
    }
    return bytes;
}

[[nodiscard]] std::expected<ImageFacts, std::string> inspect_file(std::span<const std::byte> file) {
    const auto dos_header = read_value<IMAGE_DOS_HEADER>(file, 0);
    if (!dos_header.has_value() || dos_header->e_magic != IMAGE_DOS_SIGNATURE || dos_header->e_lfanew < 0) {
        return std::unexpected("file has no valid DOS header");
    }

    const auto nt_offset = static_cast<std::size_t>(dos_header->e_lfanew);
    const auto signature = read_value<DWORD>(file, nt_offset);
    if (!signature.has_value() || *signature != IMAGE_NT_SIGNATURE) {
        return std::unexpected("file has no valid PE signature");
    }

    const auto file_header_offset = nt_offset + sizeof(DWORD);
    const auto file_header = read_value<IMAGE_FILE_HEADER>(file, file_header_offset);
    if (!file_header.has_value() || file_header->NumberOfSections == 0) {
        return std::unexpected("PE file header is missing or invalid");
    }

    const auto optional_header_offset = file_header_offset + sizeof(IMAGE_FILE_HEADER);
    const auto magic = read_value<WORD>(file, optional_header_offset);
    if (!magic.has_value()) {
        return std::unexpected("PE optional header is missing");
    }

    if constexpr (sizeof(void*) == 4) {
        if (file_header->Machine != IMAGE_FILE_MACHINE_I386 || *magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
            return std::unexpected("PE architecture does not match the x86 verifier");
        }
        return read_image_facts<IMAGE_OPTIONAL_HEADER32>(file, *file_header, optional_header_offset);
    } else {
        if (file_header->Machine != IMAGE_FILE_MACHINE_AMD64 || *magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
            return std::unexpected("PE architecture does not match the x64 verifier");
        }
        return read_image_facts<IMAGE_OPTIONAL_HEADER64>(file, *file_header, optional_header_offset);
    }
}

[[nodiscard]] std::expected<void, std::string> map_sections(std::span<const std::byte> file, const ImageFacts& facts,
                                                            std::span<std::byte> image) {
    if (!contains(file, 0, facts.header_size) || facts.header_size > image.size()) {
        return std::unexpected("PE headers lie outside the file or mapped image");
    }
    std::memcpy(image.data(), file.data(), facts.header_size);

    for (std::size_t index = 0; index < facts.section_count; ++index) {
        if (index > (std::numeric_limits<std::size_t>::max() - facts.section_table) / sizeof(IMAGE_SECTION_HEADER)) {
            return std::unexpected("PE section header offset overflows");
        }
        const auto section_offset = facts.section_table + index * sizeof(IMAGE_SECTION_HEADER);
        const auto section = read_value<IMAGE_SECTION_HEADER>(file, section_offset);
        if (!section.has_value()) {
            return std::unexpected("PE section header lies outside the file");
        }

        const auto virtual_address = static_cast<std::size_t>(section->VirtualAddress);
        const auto virtual_size = static_cast<std::size_t>(section->Misc.VirtualSize);
        const auto raw_offset = static_cast<std::size_t>(section->PointerToRawData);
        const auto raw_size = static_cast<std::size_t>(section->SizeOfRawData);
        if (!contains(image, virtual_address, std::max(virtual_size, raw_size)) ||
            !contains(file, raw_offset, raw_size)) {
            return std::unexpected("PE section lies outside the file or mapped image");
        }
        if (raw_size != 0) {
            std::memcpy(image.data() + virtual_address, file.data() + raw_offset, raw_size);
        }
    }
    return {};
}

} // namespace

MappedImage::MappedImage(void* allocation, std::size_t size, std::uintptr_t preferred_base) noexcept
    : allocation_(allocation), size_(size), preferred_base_(preferred_base) {}

void MappedImage::AllocationDeleter::operator()(void* allocation) const noexcept {
    if (allocation != nullptr) {
        static_cast<void>(VirtualFree(allocation, 0, MEM_RELEASE));
    }
}

std::expected<MappedImage, std::string> MappedImage::load(const std::filesystem::path& path) {
    auto file = read_file(path);
    if (!file.has_value()) {
        return std::unexpected(std::move(file.error()));
    }
    auto facts = inspect_file(*file);
    if (!facts.has_value()) {
        return std::unexpected(std::move(facts.error()));
    }

    auto* allocation = VirtualAlloc(reinterpret_cast<void*>(facts->preferred_base), facts->size,
                                    MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (allocation == nullptr) {
        // Fixed-base x86 images can overlap the verifier's process heap, so validation supports a separate copy.
        allocation = VirtualAlloc(nullptr, facts->size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    }
    if (allocation == nullptr) {
        return std::unexpected("could not reserve private memory for the mapped image (Windows error " +
                               std::to_string(GetLastError()) + ")");
    }

    MappedImage mapped(allocation, facts->size, facts->preferred_base);
    if (auto result = map_sections(*file, *facts, mapped.bytes()); !result.has_value()) {
        return std::unexpected(std::move(result.error()));
    }
    return mapped;
}

std::uintptr_t MappedImage::base() const noexcept {
    return reinterpret_cast<std::uintptr_t>(allocation_.get());
}

std::uintptr_t MappedImage::preferred_base() const noexcept {
    return preferred_base_;
}

std::span<std::byte> MappedImage::bytes() noexcept {
    return {static_cast<std::byte*>(allocation_.get()), size_};
}

} // namespace fusioncutter::verify
