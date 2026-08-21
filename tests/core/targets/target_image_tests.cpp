#include "recognition.hpp"

#include <Windows.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace {

// Bounded copies avoid unaligned typed access while the fixture walks untrusted on-disk PE offsets.
template <class Value>
[[nodiscard]] std::optional<Value> read_at(std::span<const std::byte> bytes, std::size_t offset) noexcept {
    if (offset > bytes.size() || sizeof(Value) > bytes.size() - offset) {
        return std::nullopt;
    }
    Value result{};
    std::memcpy(&result, bytes.data() + offset, sizeof(result));
    return result;
}

// Recreates the loader's virtual image layout from reviewed on-disk bytes without executing the binary.
[[nodiscard]] std::optional<std::vector<std::byte>> map_image(const std::filesystem::path& path) {
    std::ifstream input{path, std::ios::binary | std::ios::ate};
    if (!input) {
        return std::nullopt;
    }
    const auto end = input.tellg();
    if (end <= 0) {
        return std::nullopt;
    }
    std::vector<std::byte> file(static_cast<std::size_t>(end));
    input.seekg(0);
    input.read(reinterpret_cast<char*>(file.data()), static_cast<std::streamsize>(file.size()));
    if (!input) {
        return std::nullopt;
    }

    // Validate the on-disk header chain before using any offset or allocation size read from the file.
    const auto dos = read_at<IMAGE_DOS_HEADER>(file, 0);
    if (!dos || dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0) {
        return std::nullopt;
    }
    const auto nt_offset = static_cast<std::size_t>(dos->e_lfanew);
    const auto signature = read_at<DWORD>(file, nt_offset);
    const auto header = read_at<IMAGE_FILE_HEADER>(file, nt_offset + sizeof(DWORD));
    if (!signature || *signature != IMAGE_NT_SIGNATURE || !header) {
        return std::nullopt;
    }
    const auto optional_offset = nt_offset + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER);
    const auto magic = read_at<WORD>(file, optional_offset);
    std::uint32_t image_size{};
    std::uint32_t headers_size{};
    if (magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
        const auto optional = read_at<IMAGE_OPTIONAL_HEADER32>(file, optional_offset);
        if (!optional) {
            return std::nullopt;
        }
        image_size = optional->SizeOfImage;
        headers_size = optional->SizeOfHeaders;
    } else if (magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        const auto optional = read_at<IMAGE_OPTIONAL_HEADER64>(file, optional_offset);
        if (!optional) {
            return std::nullopt;
        }
        image_size = optional->SizeOfImage;
        headers_size = optional->SizeOfHeaders;
    } else {
        return std::nullopt;
    }
    if (image_size == 0 || headers_size > image_size || headers_size > file.size()) {
        return std::nullopt;
    }

    // Copy headers and raw section extents to their RVAs; zero-filled virtual tails remain zero initialized.
    std::vector<std::byte> mapped(image_size);
    std::ranges::copy(std::span{file}.first(headers_size), mapped.begin());
    const auto sections_offset = optional_offset + header->SizeOfOptionalHeader;
    for (std::size_t index = 0; index < header->NumberOfSections; ++index) {
        const auto section =
            read_at<IMAGE_SECTION_HEADER>(file, sections_offset + index * sizeof(IMAGE_SECTION_HEADER));
        if (!section || section->PointerToRawData > file.size() ||
            section->SizeOfRawData > file.size() - section->PointerToRawData ||
            section->VirtualAddress > mapped.size()) {
            return std::nullopt;
        }
        const auto copy_size = std::min<std::size_t>(section->SizeOfRawData, mapped.size() - section->VirtualAddress);
        std::ranges::copy(std::span{file}.subspan(section->PointerToRawData, copy_size),
                          mapped.begin() + section->VirtualAddress);
    }
    return mapped;
}

} // namespace

int main(int argument_count, char** arguments) {
    // Accepts an image path so CTest can register each locally available reviewed image independently.
    if (argument_count != 3) {
        std::cerr << "Expected profile ID and reviewed image path\n";
        return 2;
    }
    const std::filesystem::path path{arguments[2]};
    auto image = map_image(path);
    if (!image) {
        std::cerr << "Could not map reviewed PE image: " << path << '\n';
        return 1;
    }
    const auto basename = path.filename().string();
    const auto recognized = fc::targets::recognize_mapped_image(basename, *image);
    if (!recognized) {
        std::cerr << recognized.error().message << '\n';
        return 1;
    }
    if (recognized->profile->id != arguments[1]) {
        std::cerr << "Recognized profile " << recognized->profile->id << " instead of " << arguments[1] << '\n';
        return 1;
    }
    return 0;
}
