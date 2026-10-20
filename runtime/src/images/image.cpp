#include "image.hpp"

#define PSAPI_VERSION 2
#include <Windows.h>
#include <Psapi.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <fc/base.hpp>
#include <limits>
#include <memory>
#include <type_traits>
#include <unordered_set>
#include <utility>

namespace fc::runtime {

namespace {

constexpr std::uint16_t kDosSignature = 0x5a4d;
constexpr std::uint32_t kPeSignature = 0x00004550;
constexpr std::uint16_t kX86Machine = 0x014c;
constexpr std::uint16_t kX64Machine = 0x8664;
constexpr std::uint16_t kPe32Magic = 0x010b;
constexpr std::uint16_t kPe32PlusMagic = 0x020b;
constexpr std::size_t kDosHeaderSize = 64;
constexpr std::size_t kPeFileHeaderSize = 20;
constexpr std::size_t kSectionHeaderSize = 40;

#if defined(_WIN64)
constexpr std::uint16_t kExpectedMachine = kX64Machine;
constexpr std::uint16_t kExpectedMagic = kPe32PlusMagic;
#else
constexpr std::uint16_t kExpectedMachine = kX86Machine;
constexpr std::uint16_t kExpectedMagic = kPe32Magic;
#endif

// Check the range using subtraction to avoid overflow when adding offset and length.
bool has_range(std::size_t size, std::size_t offset, std::size_t length) {
    return offset <= size && length <= size - offset;
}

// Reads a field that may be unaligned. The caller must first check that its bytes are present.
template <class T> T read_value(std::span<const std::byte> bytes, std::size_t offset) {
    T value{};
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

// Parses PE headers for this runtime's architecture. file_layout selects raw file range checks;
// mapped input must contain the full declared SizeOfImage instead.
std::expected<ImageLayout, Diagnostic> parse_image(std::span<const std::byte> bytes, std::string_view owner, bool file_layout) {
    const std::string diagnostic_owner{owner};

    // The DOS header identifies the format and stores the offset of the PE header at byte 0x3c.
    if (!has_range(bytes.size(), 0, kDosHeaderSize)) {
        return std::unexpected{Diagnostic{diagnostic_owner, "truncated DOS header"}};
    }
    if (read_value<std::uint16_t>(bytes, 0) != kDosSignature) {
        return std::unexpected{Diagnostic{diagnostic_owner, "invalid DOS signature"}};
    }

    const auto pe_offset = static_cast<std::size_t>(read_value<std::uint32_t>(bytes, 0x3c));
    if (!has_range(bytes.size(), pe_offset, 4 + kPeFileHeaderSize)) {
        return std::unexpected{Diagnostic{diagnostic_owner, "truncated PE file header"}};
    }
    if (read_value<std::uint32_t>(bytes, pe_offset) != kPeSignature) {
        return std::unexpected{Diagnostic{diagnostic_owner, "invalid PE signature"}};
    }

    // The PE file header identifies the CPU architecture and the sizes needed to locate later headers.
    const auto file_header = pe_offset + 4;
    const auto machine = read_value<std::uint16_t>(bytes, file_header);
    if (machine != kExpectedMachine) {
        return std::unexpected{Diagnostic{diagnostic_owner, "PE machine does not match this runtime's architecture"}};
    }

    const auto section_count = read_value<std::uint16_t>(bytes, file_header + 2);
    const auto optional_size = static_cast<std::size_t>(read_value<std::uint16_t>(bytes, file_header + 16));
    const auto optional_offset = file_header + kPeFileHeaderSize;
    if (!has_range(bytes.size(), optional_offset, optional_size)) {
        return std::unexpected{Diagnostic{diagnostic_owner, "truncated PE optional header"}};
    }

    // PE32 and PE32+ use different optional header layouts; the magic selects the layout for this architecture.
    if (optional_size < 2) {
        return std::unexpected{Diagnostic{diagnostic_owner, "truncated PE optional header"}};
    }
    const auto magic = read_value<std::uint16_t>(bytes, optional_offset);
    if (magic != kExpectedMagic) {
        return std::unexpected{Diagnostic{diagnostic_owner, "PE optional header magic does not match this runtime's architecture"}};
    }

    // Require the fixed optional fields before reading image sizes and ImageBase at their PE offsets.
    const std::size_t fixed_optional_size = magic == kPe32Magic ? 96 : 112;
    if (optional_size < fixed_optional_size) {
        return std::unexpected{Diagnostic{diagnostic_owner, "truncated fixed PE optional header"}};
    }

    // SizeOfImage bounds the mapped sections, while SizeOfHeaders bounds the header tables.
    const auto image_size = read_value<std::uint32_t>(bytes, optional_offset + 56);
    const auto headers_size = read_value<std::uint32_t>(bytes, optional_offset + 60);
    if (image_size == 0) {
        return std::unexpected{Diagnostic{diagnostic_owner, "PE SizeOfImage is zero"}};
    }
    if (headers_size > image_size) {
        return std::unexpected{Diagnostic{diagnostic_owner, "PE SizeOfHeaders exceeds SizeOfImage"}};
    }

    // A loaded module must supply its full declared memory extent; file bytes use a different section layout.
    if (!file_layout && image_size > bytes.size()) {
        return std::unexpected{Diagnostic{diagnostic_owner, "mapped PE bytes are shorter than SizeOfImage"}};
    }

    // SizeOfOptionalHeader locates the section table even when the header contains extra data directories.
    const auto section_table = optional_offset + optional_size;
    const auto section_bytes = static_cast<std::size_t>(section_count) * kSectionHeaderSize;
    if (!has_range(bytes.size(), section_table, section_bytes)) {
        return std::unexpected{Diagnostic{diagnostic_owner, "truncated PE section table"}};
    }
    if (section_table > headers_size || section_bytes > headers_size - section_table) {
        return std::unexpected{Diagnostic{diagnostic_owner, "PE section table exceeds SizeOfHeaders"}};
    }

    // ImageBase is a 32-bit field in PE32 and a 64-bit field at a different offset in PE32+.
    ImageLayout layout{image_size, {}};
    layout.preferred_base = magic == kPe32Magic ? read_value<std::uint32_t>(bytes, optional_offset + 28)
                                                : read_value<std::uint64_t>(bytes, optional_offset + 24);
    layout.sections.reserve(section_count);
    for (std::uint16_t index = 0; index < section_count; ++index) {
        const auto offset = section_table + static_cast<std::size_t>(index) * kSectionHeaderSize;

        // Section names occupy eight bytes and can fill all eight without a terminating null.
        std::array<char, 8> name_bytes{};
        std::memcpy(name_bytes.data(), bytes.data() + offset, name_bytes.size());
        const auto name_end = std::find(name_bytes.begin(), name_bytes.end(), '\0');

        // Retain both file offsets and memory offsets so recognition and offline mapping can use the same layout.
        ImageSection section;
        section.name.assign(name_bytes.begin(), name_end);
        section.virtual_size = read_value<std::uint32_t>(bytes, offset + 8);
        section.rva = read_value<std::uint32_t>(bytes, offset + 12);
        section.raw_size = read_value<std::uint32_t>(bytes, offset + 16);
        section.raw_offset = read_value<std::uint32_t>(bytes, offset + 20);
        section.characteristics = read_value<std::uint32_t>(bytes, offset + 36);

        // Use SizeOfRawData for the section's byte count in memory when VirtualSize is zero.
        section.mapped_size = section.virtual_size == 0 ? section.raw_size : section.virtual_size;

        // Each section's RVA and memory byte count must fit inside the declared memory image.
        if (section.mapped_size > image_size || section.rva > image_size - section.mapped_size) {
            return std::unexpected{Diagnostic{diagnostic_owner, "section '" + section.name + "' mapped range is outside SizeOfImage"}};
        }

        // Raw offsets locate section bytes in the file. Loaded sections use RVAs relative to the image base.
        if (file_layout && section.raw_size != 0 &&
            (section.raw_offset > bytes.size() || section.raw_size > bytes.size() - section.raw_offset)) {
            return std::unexpected{Diagnostic{diagnostic_owner, "section '" + section.name + "' range of file data is outside the file"}};
        }

        // Check that each section occupies a separate range of image memory.
        if (section.mapped_size != 0) {
            for (const auto &existing : layout.sections) {
                if (existing.mapped_size != 0 && section.rva < existing.rva + existing.mapped_size &&
                    existing.rva < section.rva + section.mapped_size) {
                    return std::unexpected{Diagnostic{diagnostic_owner, "section '" + section.name + "' has an overlapping mapped range"}};
                }
            }
        }

        layout.sections.push_back(std::move(section));
    }

    return layout;
}

// Reads the module's full path, growing the buffer until Windows returns the complete name.
std::expected<std::wstring, Diagnostic> module_path(HMODULE module, std::string_view owner) {
    std::vector<wchar_t> buffer(MAX_PATH);
    for (;;) {
        SetLastError(ERROR_SUCCESS);
        const auto length = GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            const auto error = GetLastError();
            return std::unexpected{Diagnostic{std::string{owner}, "could not read loaded module path", error}};
        }
        if (length < buffer.size()) {
            return std::wstring{buffer.data(), length};
        }
        if (buffer.size() > std::numeric_limits<DWORD>::max() / 2) {
            return std::unexpected{Diagnostic{std::string{owner}, "loaded module path is too long to read"}};
        }
        buffer.resize(buffer.size() * 2);
    }
}

// Convert a Windows filename to UTF-8 for comparison with catalog filenames.
std::string narrow_utf8(std::wstring_view value) {
    if (value.empty()) {
        return {};
    }

    // Measure the UTF-8 output first because its byte count can differ from the wide character count.
    const auto length = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), length, nullptr, nullptr);
    return result;
}

// Lists the modules loaded in this process, retrying if the list grows while it is being read.
std::expected<std::vector<HMODULE>, Diagnostic> enumerate_modules(std::string_view owner) {
    DWORD needed{};
    if (K32EnumProcessModules(GetCurrentProcess(), nullptr, 0, &needed) == 0) {
        const auto error = GetLastError();
        return std::unexpected{Diagnostic{std::string{owner}, "could not enumerate loaded modules", error}};
    }

    // Windows reports the required size in bytes; round up to a whole number of module handles.
    std::vector<HMODULE> modules((needed + sizeof(HMODULE) - 1) / sizeof(HMODULE));
    for (;;) {
        if (K32EnumProcessModules(GetCurrentProcess(), modules.data(), static_cast<DWORD>(modules.size() * sizeof(HMODULE)), &needed) ==
            0) {
            const auto error = GetLastError();
            return std::unexpected{Diagnostic{std::string{owner}, "could not enumerate loaded modules", error}};
        }

        // Once the list fits, retain only the handles written; otherwise resize using the new byte count and retry.
        if (needed <= modules.size() * sizeof(HMODULE)) {
            modules.resize(needed / sizeof(HMODULE));
            return modules;
        }
        modules.resize((needed + sizeof(HMODULE) - 1) / sizeof(HMODULE));
    }
}

// Inspects a module held by the caller or the process and pins a recognized mapping.
std::expected<LoadedImage, Diagnostic> inspect_module(HMODULE held, std::string_view actual_filename,
                                                      std::span<const ImageProfile> catalog) {
    // Obtain the selected module's actual base address and memory size for header inspection.
    MODULEINFO information{};
    if (K32GetModuleInformation(GetCurrentProcess(), held, &information, sizeof(information)) == 0) {
        const auto error = GetLastError();
        return std::unexpected{Diagnostic{std::string{actual_filename}, "could not read loaded module information", error}};
    }

    // Inspect the headers in the loader's memory span to recover the section RVAs and declared image size.
    const auto mapped_bytes =
        std::span{static_cast<const std::byte *>(information.lpBaseOfDll), static_cast<std::size_t>(information.SizeOfImage)};
    auto layout = parse_image(mapped_bytes, actual_filename, false);
    if (!layout) {
        return std::unexpected{std::move(layout.error())};
    }

    // Recognize the selected module's PE facts, retaining its actual basename for diagnostics.
    auto profile = recognize_image(actual_filename, *layout, catalog);
    if (!profile) {
        return std::unexpected{std::move(profile.error())};
    }

    // Pin the recognized module until process exit; temporary inspection references can then be released.
    HMODULE pinned{};
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                           reinterpret_cast<LPCWSTR>(information.lpBaseOfDll), &pinned) == 0 ||
        pinned != held) {
        const auto error = GetLastError();
        return std::unexpected{Diagnostic{std::string{actual_filename}, "could not pin recognized module until process exit", error}};
    }

    LoadedImage image;
    image.profile = *profile;
    image.filename = actual_filename;
    image.base = reinterpret_cast<std::uintptr_t>(information.lpBaseOfDll);
    image.image_size = layout->image_size;
    image.sections.reserve(layout->sections.size());

    // Add each section RVA to the actual loaded base, which can differ from the preferred ImageBase in the headers.
    for (const auto &section : layout->sections) {
        image.sections.push_back({section.name, image.base + section.rva, section.mapped_size, section.characteristics});
    }

    return image;
}

// Releases temporary loader references after inspection; successful matches are separately pinned until exit.
using ModuleReference = std::unique_ptr<std::remove_pointer_t<HMODULE>, decltype(&FreeLibrary)>;

} // namespace

std::expected<ImageLayout, Diagnostic> parse_file_image(std::span<const std::byte> bytes, std::string_view owner) {
    return parse_image(bytes, owner, true);
}

// Rejects duplicate profile IDs under the same ASCII case-insensitive comparison used by patch selection.
std::optional<Diagnostic> validate_image_catalog(std::span<const ImageProfile> catalog) {
    std::unordered_set<std::string> ids;
    for (const auto &profile : catalog) {
        if (!ids.insert(detail::folded_id(profile.id)).second) {
            return Diagnostic{"image catalog",
                              "duplicate profile ID '" + std::string{profile.id} + "' under ASCII case-insensitive comparison"};
        }
    }

    return std::nullopt;
}

// Finds the single build whose identifying section facts match the inspected image.
std::expected<const ImageProfile *, Diagnostic> recognize_image(std::string_view filename, const ImageLayout &layout,
                                                                std::span<const ImageProfile> catalog) {
    const ImageProfile *match{};
    for (const auto &profile : catalog) {
        // Image size is a minimum; filenames provide context without restricting build recognition.
        if (layout.image_size < profile.image_size) {
            continue;
        }

        // Each required name, RVA, and VirtualSize must match exactly one section; extra sections are allowed.
        const auto sections_match = std::ranges::all_of(profile.sections, [&](const SectionProfile &expected) {
            return std::ranges::count_if(layout.sections, [&](const ImageSection &actual) {
                       return actual.name == expected.name && actual.rva == expected.rva && actual.virtual_size == expected.size;
                   }) == 1;
        });
        if (sections_match) {
            if (match) {
                return std::unexpected{Diagnostic{std::string{filename}, "image recognition is ambiguous across multiple profiles"}};
            }
            match = &profile;
        }
    }

    if (!match) {
        return std::unexpected{Diagnostic{std::string{filename}, "profile catalog has no matching entry for this image"}};
    }
    return match;
}

std::expected<const ImageProfile *, Diagnostic> find_image_profile(std::string_view id, std::span<const ImageProfile> catalog) {
    for (const auto &profile : catalog) {
        if (detail::equal_id(profile.id, id)) {
            return &profile;
        }
    }

    return std::unexpected{Diagnostic{"image catalog", "unknown image profile ID '" + std::string{id} + "'"}};
}

std::expected<std::optional<LoadedImage>, Diagnostic> probe_loaded_image(std::string_view filename, std::span<const ImageProfile> catalog) {
    if (filename.empty()) {
        return std::unexpected{Diagnostic{"loaded module", "module filename is empty"}};
    }

    // A requested basename must belong to some profile before its absence can be treated as an expected result.
    const auto accepted_filename = std::ranges::any_of(catalog, [&](const ImageProfile &profile) {
        return std::ranges::any_of(profile.filenames, [&](std::string_view accepted) {
            return detail::equal_id(accepted, filename);
        });
    });
    if (!accepted_filename) {
        return std::unexpected{Diagnostic{std::string{filename}, "profile catalog has no entry for this module filename"}};
    }

    // Enumerate the process modules so duplicate basenames can be detected across different paths.
    auto modules = enumerate_modules(filename);
    if (!modules) {
        return std::unexpected{std::move(modules.error())};
    }

    // Retain the first matching module reference with its actual basename while checking for duplicates.
    struct Match {
        ModuleReference module;
        std::string filename;
    };

    std::optional<Match> match;
    for (const auto candidate : *modules) {
        // Acquire a temporary loader reference before reading the candidate's path or memory.
        HMODULE held{};
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(candidate), &held) == 0) {
            const auto error = GetLastError();
            if (error == ERROR_MOD_NOT_FOUND) {
                // A module can unload between enumeration and acquiring its reference.
                continue;
            }
            return std::unexpected{Diagnostic{std::string{filename}, "could not acquire a loaded module reference", error}};
        }

        ModuleReference reference{held, &FreeLibrary};
        auto path = module_path(reference.get(), filename);
        if (!path) {
            return std::unexpected{std::move(path.error())};
        }

        // A second matching basename is ambiguous even when the modules came from different directories.
        auto basename = narrow_utf8(std::filesystem::path{*path}.filename().native());
        if (detail::equal_id(basename, filename)) {
            if (match) {
                return std::unexpected{Diagnostic{std::string{filename}, "more than one loaded module has the requested basename"}};
            }
            match.emplace(std::move(reference), std::move(basename));
        }
    }

    if (!match) {
        return std::optional<LoadedImage>{};
    }

    auto image = inspect_module(match->module.get(), match->filename, catalog);
    if (!image) {
        return std::unexpected{std::move(image.error())};
    }
    return std::optional<LoadedImage>{std::move(*image)};
}

std::expected<LoadedImage, Diagnostic> probe_main_image(std::span<const ImageProfile> catalog) {
    const auto module = GetModuleHandleW(nullptr);
    if (!module) {
        return std::unexpected{Diagnostic{"main executable", "could not retrieve the process executable", GetLastError()}};
    }
    auto path = module_path(module, "main executable");
    if (!path) {
        return std::unexpected{std::move(path.error())};
    }
    const auto filename = narrow_utf8(std::filesystem::path{*path}.filename().native());
    return inspect_module(module, filename, catalog);
}

} // namespace fc::runtime
