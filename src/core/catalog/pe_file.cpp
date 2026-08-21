#include "pe_file.hpp"

#include <Windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

namespace fc::catalog {
namespace {

constexpr std::string_view kQueryName = "FusionCutter_QueryPlugin";

// Move-only owner for the read-only file and mapping inspected before a plugin candidate may execute.
class MappedFile {
  public:
    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;

    MappedFile(MappedFile&& other) noexcept
        : file_(std::exchange(other.file_, INVALID_HANDLE_VALUE)), mapping_(std::exchange(other.mapping_, nullptr)),
          data_(std::exchange(other.data_, nullptr)), size_(std::exchange(other.size_, 0)) {}

    MappedFile& operator=(MappedFile&&) = delete;

    ~MappedFile() {
        if (data_ != nullptr) {
            UnmapViewOfFile(data_);
        }
        if (mapping_ != nullptr) {
            CloseHandle(mapping_);
        }
        if (file_ != INVALID_HANDLE_VALUE) {
            CloseHandle(file_);
        }
    }

    // Opens a shareable read-only snapshot without granting execute access or participating in DLL search.
    [[nodiscard]] static std::expected<MappedFile, std::string> open(const std::filesystem::path& path) {
        const auto file =
            CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            return std::unexpected("The candidate file could not be opened");
        }

        LARGE_INTEGER file_size{};
        if (!GetFileSizeEx(file, &file_size) || file_size.QuadPart <= 0 ||
            static_cast<unsigned long long>(file_size.QuadPart) > std::numeric_limits<std::size_t>::max()) {
            CloseHandle(file);
            return std::unexpected("The candidate file has an invalid size");
        }

        const auto mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (mapping == nullptr) {
            CloseHandle(file);
            return std::unexpected("The candidate file could not be mapped");
        }
        const auto data = static_cast<const std::byte*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0));
        if (data == nullptr) {
            CloseHandle(mapping);
            CloseHandle(file);
            return std::unexpected("The candidate file view could not be created");
        }
        return MappedFile{file, mapping, data, static_cast<std::size_t>(file_size.QuadPart)};
    }

    [[nodiscard]] std::span<const std::byte> bytes() const noexcept {
        return {data_, size_};
    }

  private:
    MappedFile(HANDLE file, HANDLE mapping, const std::byte* data, std::size_t size) noexcept
        : file_(file), mapping_(mapping), data_(data), size_(size) {}

    HANDLE file_{INVALID_HANDLE_VALUE};
    HANDLE mapping_{};
    const std::byte* data_{};
    std::size_t size_{};
};

// Typed views are returned only when the complete object is both in-bounds and naturally aligned.
template <class T> [[nodiscard]] const T* view_at(std::span<const std::byte> bytes, std::size_t offset) noexcept {
    if (offset > bytes.size() || sizeof(T) > bytes.size() - offset ||
        reinterpret_cast<std::uintptr_t>(bytes.data() + offset) % alignof(T) != 0) {
        return nullptr;
    }
    return reinterpret_cast<const T*>(bytes.data() + offset);
}

[[nodiscard]] bool checked_multiply(std::size_t left, std::size_t right, std::size_t& result) noexcept {
    if (left != 0 && right > std::numeric_limits<std::size_t>::max() / left) {
        return false;
    }
    result = left * right;
    return true;
}

[[nodiscard]] bool checked_add(std::size_t left, std::size_t right, std::size_t& result) noexcept {
    if (right > std::numeric_limits<std::size_t>::max() - left) {
        return false;
    }
    result = left + right;
    return true;
}

// Converts bounded on-disk RVAs to raw bytes; virtual-only section tails are never treated as file data.
struct PeView {
    std::span<const std::byte> bytes;
    const IMAGE_FILE_HEADER* file_header{};
    const IMAGE_SECTION_HEADER* sections{};
    DWORD size_of_headers{};
    IMAGE_DATA_DIRECTORY export_directory{};

    [[nodiscard]] std::optional<std::size_t> raw_offset(DWORD rva, std::size_t size) const noexcept {
        // Header RVAs are direct; section RVAs translate through raw extents and reject virtual-only tails.
        if (rva < size_of_headers) {
            const auto offset = static_cast<std::size_t>(rva);
            if (offset <= bytes.size() && size <= bytes.size() - offset) {
                return offset;
            }
        }

        for (WORD index = 0; index < file_header->NumberOfSections; ++index) {
            const auto& section = sections[index];
            const auto virtual_size = std::max(section.Misc.VirtualSize, section.SizeOfRawData);
            if (rva < section.VirtualAddress || rva - section.VirtualAddress >= virtual_size) {
                continue;
            }
            const auto delta = static_cast<std::size_t>(rva - section.VirtualAddress);
            if (delta > section.SizeOfRawData || size > section.SizeOfRawData - delta) {
                return std::nullopt;
            }
            std::size_t offset{};
            if (!checked_add(section.PointerToRawData, delta, offset) || offset > bytes.size() ||
                size > bytes.size() - offset) {
                return std::nullopt;
            }
            return offset;
        }
        return std::nullopt;
    }

    template <class T> [[nodiscard]] const T* rva_view(DWORD rva, std::size_t count = 1) const noexcept {
        std::size_t size{};
        if (!checked_multiply(sizeof(T), count, size)) {
            return nullptr;
        }
        const auto offset = raw_offset(rva, size);
        return offset ? view_at<T>(bytes, *offset) : nullptr;
    }

    [[nodiscard]] std::optional<std::string_view> rva_string(DWORD rva) const noexcept {
        const auto offset = raw_offset(rva, 1);
        if (!offset) {
            return std::nullopt;
        }
        const auto* begin = reinterpret_cast<const char*>(bytes.data() + *offset);
        const auto remaining = bytes.size() - *offset;
        const auto* end = static_cast<const char*>(std::memchr(begin, '\0', remaining));
        if (end == nullptr) {
            return std::nullopt;
        }
        return std::string_view{begin, static_cast<std::size_t>(end - begin)};
    }
};

// Builds a bounded non-owning PE view after validating every header and section table extent it retains.
[[nodiscard]] std::expected<std::pair<FC_Architecture, PeView>, std::string> read_pe(std::span<const std::byte> bytes) {
    const auto* dos = view_at<IMAGE_DOS_HEADER>(bytes, 0);
    if (dos == nullptr || dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) {
        return std::unexpected("The candidate has no valid DOS header");
    }
    const auto nt_offset = static_cast<std::size_t>(dos->e_lfanew);
    const auto* signature = view_at<DWORD>(bytes, nt_offset);
    if (signature == nullptr || *signature != IMAGE_NT_SIGNATURE) {
        return std::unexpected("The candidate has no valid PE signature");
    }

    std::size_t file_header_offset{};
    if (!checked_add(nt_offset, sizeof(DWORD), file_header_offset)) {
        return std::unexpected("The candidate PE header offset overflows");
    }
    const auto* file_header = view_at<IMAGE_FILE_HEADER>(bytes, file_header_offset);
    if (file_header == nullptr || file_header->NumberOfSections == 0) {
        return std::unexpected("The candidate PE file header is truncated or empty");
    }

    // Architecture is derived from the COFF machine, then checked against a compatible optional header format.
    FC_Architecture architecture{};
    if (file_header->Machine == IMAGE_FILE_MACHINE_I386) {
        architecture = FC_ARCH_X86;
    } else if (file_header->Machine == IMAGE_FILE_MACHINE_AMD64) {
        architecture = FC_ARCH_X64;
    } else {
        return std::unexpected("The candidate PE machine is unsupported");
    }

    std::size_t optional_offset{};
    if (!checked_add(file_header_offset, sizeof(IMAGE_FILE_HEADER), optional_offset)) {
        return std::unexpected("The candidate optional header offset overflows");
    }
    if (optional_offset > bytes.size() || file_header->SizeOfOptionalHeader > bytes.size() - optional_offset) {
        return std::unexpected("The candidate optional header is truncated");
    }

    const auto* magic = view_at<WORD>(bytes, optional_offset);
    DWORD size_of_headers{};
    IMAGE_DATA_DIRECTORY export_directory{};
    if (magic != nullptr && *magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC &&
        file_header->SizeOfOptionalHeader >= sizeof(IMAGE_OPTIONAL_HEADER32)) {
        if (architecture != FC_ARCH_X86) {
            return std::unexpected("The candidate PE machine and optional header architecture disagree");
        }
        const auto* optional = view_at<IMAGE_OPTIONAL_HEADER32>(bytes, optional_offset);
        size_of_headers = optional->SizeOfHeaders;
        if (optional->NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_EXPORT) {
            export_directory = optional->DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        }
    } else if (magic != nullptr && *magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC &&
               file_header->SizeOfOptionalHeader >= sizeof(IMAGE_OPTIONAL_HEADER64)) {
        if (architecture != FC_ARCH_X64) {
            return std::unexpected("The candidate PE machine and optional header architecture disagree");
        }
        const auto* optional = view_at<IMAGE_OPTIONAL_HEADER64>(bytes, optional_offset);
        size_of_headers = optional->SizeOfHeaders;
        if (optional->NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_EXPORT) {
            export_directory = optional->DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        }
    } else {
        return std::unexpected("The candidate optional header format is unsupported or truncated");
    }

    // The retained section pointer is published only after the entire table is proven to lie in the mapping.
    std::size_t section_offset{};
    if (!checked_add(optional_offset, file_header->SizeOfOptionalHeader, section_offset)) {
        return std::unexpected("The candidate section table offset overflows");
    }
    std::size_t section_size{};
    if (!checked_multiply(file_header->NumberOfSections, sizeof(IMAGE_SECTION_HEADER), section_size) ||
        section_offset > bytes.size() || section_size > bytes.size() - section_offset) {
        return std::unexpected("The candidate section table is truncated");
    }
    const auto* sections = reinterpret_cast<const IMAGE_SECTION_HEADER*>(bytes.data() + section_offset);
    return std::pair{architecture, PeView{bytes, file_header, sections, size_of_headers, export_directory}};
}

// Resolves the exact query export through bounded name, ordinal, and function arrays without loading the image.
[[nodiscard]] std::expected<QueryExportKind, std::string> inspect_query_export(const PeView& pe) {
    const auto& directory = pe.export_directory;
    if (directory.VirtualAddress == 0 || directory.Size == 0) {
        return QueryExportKind::Missing;
    }
    const auto* exports = pe.rva_view<IMAGE_EXPORT_DIRECTORY>(directory.VirtualAddress);
    if (exports == nullptr) {
        return std::unexpected("The candidate export directory is malformed");
    }
    const auto* names = pe.rva_view<DWORD>(exports->AddressOfNames, exports->NumberOfNames);
    const auto* ordinals = pe.rva_view<WORD>(exports->AddressOfNameOrdinals, exports->NumberOfNames);
    const auto* functions = pe.rva_view<DWORD>(exports->AddressOfFunctions, exports->NumberOfFunctions);
    if ((exports->NumberOfNames != 0 && (names == nullptr || ordinals == nullptr)) ||
        (exports->NumberOfFunctions != 0 && functions == nullptr)) {
        return std::unexpected("The candidate export arrays are malformed");
    }

    // Search exact case-sensitive export spelling, then classify its function RVA without resolving or executing it.
    for (DWORD index = 0; index < exports->NumberOfNames; ++index) {
        const auto name = pe.rva_string(names[index]);
        if (!name) {
            return std::unexpected("The candidate contains a malformed export name");
        }
        if (*name != kQueryName) {
            continue;
        }
        if (ordinals[index] >= exports->NumberOfFunctions) {
            return std::unexpected("The plugin query export has an invalid ordinal");
        }
        const auto function_rva = functions[ordinals[index]];
        const auto export_begin = static_cast<std::uint64_t>(directory.VirtualAddress);
        const auto export_end = export_begin + directory.Size;
        // The PE format encodes a forwarded export as a function RVA that points back into the export directory.
        if (function_rva >= export_begin && function_rva < export_end) {
            return QueryExportKind::Forwarded;
        }
        if (function_rva == 0) {
            return std::unexpected("The plugin query export has no target");
        }
        return QueryExportKind::Direct;
    }
    return QueryExportKind::Missing;
}

} // namespace

std::expected<PluginBinaryFacts, std::string> inspect_plugin_binary(const std::filesystem::path& path) {
    auto file = MappedFile::open(path);
    if (!file) {
        return std::unexpected(std::move(file.error()));
    }
    auto pe = read_pe(file->bytes());
    if (!pe) {
        return std::unexpected(std::move(pe.error()));
    }
    auto query = inspect_query_export(pe->second);
    if (!query) {
        return std::unexpected(std::move(query.error()));
    }
    return PluginBinaryFacts{pe->first, *query};
}

} // namespace fc::catalog
