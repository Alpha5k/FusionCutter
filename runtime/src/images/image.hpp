#pragma once

#include "diagnostic.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fc::runtime {

// Describes one section that must match to recognize a supported executable or DLL build.
struct SectionProfile {
    // Section names are compared case-sensitively.
    std::string_view name;
    // Relative virtual address (RVA): the byte offset from the loaded image base.
    std::uint32_t rva{};
    // The section's PE VirtualSize must match this value exactly, including zero.
    std::uint32_t size{};
};

// Describes a supported executable or DLL build using identifying PE sections and discovery hints.
struct ImageProfile {
    // Patch requirements select this ID with ASCII case-insensitive comparison.
    std::string_view id;
    // Basenames locate supplemental DLLs using ASCII case-insensitive comparison; the main executable is inspected directly.
    std::span<const std::string_view> filenames;
    // The image must declare at least this SizeOfImage in bytes; larger images can still match.
    std::uint32_t image_size{};
    // Each required name, RVA, and VirtualSize must occur exactly once in the inspected image.
    std::span<const SectionProfile> sections;
};

// Records where a PE section resides in memory and where its data is stored in the file.
struct ImageSection {
    std::string name;
    // Relative virtual address (RVA): the byte offset from the loaded image base.
    std::uint32_t rva{};
    // The exact VirtualSize in bytes is retained for profile recognition even when zero.
    std::uint32_t virtual_size{};
    // The section occupies this many bytes in memory: VirtualSize, or SizeOfRawData when VirtualSize is zero.
    std::uint32_t mapped_size{};
    // This byte offset locates the section's stored data in the file.
    std::uint32_t raw_offset{};
    // The section stores this many bytes in the file.
    std::uint32_t raw_size{};
    // These PE flags specify whether the section is readable, writable, or executable, among other properties.
    std::uint32_t characteristics{};
};

// Describes the image size and section locations read from PE headers, for file or loaded image inspection.
struct ImageLayout {
    // SizeOfImage in bytes covers the headers and sections in memory.
    std::uint32_t image_size{};
    std::vector<ImageSection> sections;
    // Offline patch validation uses the optional header's ImageBase as the image's address.
    std::uint64_t preferred_base{};
};

// Records the recognized build and PE layout of an executable or DLL inspected from a file.
struct FileImage {
    // The catalog supplied for inspection must outlive this result.
    const ImageProfile *profile{};
    // Actual supplied basename, retained for diagnostics and reporting.
    std::string filename;
    ImageLayout layout;
};

// Describes a section's address range in a loaded module kept in memory until process exit.
struct LoadedSection {
    std::string name;
    // The section's absolute process address, computed from the module base and section RVA.
    std::uintptr_t begin{};
    // The section occupies this many bytes in memory: VirtualSize, or SizeOfRawData when VirtualSize is zero.
    std::uint32_t size{};
    // These PE flags are copied from the section header.
    std::uint32_t characteristics{};
};

// Records a recognized module's actual base address and section ranges for live patching.
struct LoadedImage {
    // The catalog supplied for inspection must outlive this result.
    const ImageProfile *profile{};
    // Basename from the module's actual path, converted to UTF-8.
    std::string filename;
    // Actual process address; probing pins the module so this remains valid until process exit.
    std::uintptr_t base{};
    // SizeOfImage in bytes, as declared in the module's PE headers.
    std::uint32_t image_size{};
    std::vector<LoadedSection> sections;
};

// Reads a PE file's headers and sections, checking their sizes, locations, and architecture.
// Returns the image layout, or a Diagnostic labeled with owner when a check fails.
std::expected<ImageLayout, Diagnostic> parse_file_image(std::span<const std::byte> bytes, std::string_view owner);

// Finds a profile by ASCII case-insensitive ID. An unknown ID returns a Diagnostic.
// Call validate_image_catalog first. The result points into the caller's catalog, which must remain alive and unchanged while used.
std::expected<const ImageProfile *, Diagnostic> find_image_profile(std::string_view id, std::span<const ImageProfile> catalog);

// Returns a Diagnostic for duplicate ASCII case-insensitive profile IDs, or nullopt when IDs are unique.
std::optional<Diagnostic> validate_image_catalog(std::span<const ImageProfile> catalog);

// Matches a parsed layout; filename labels diagnostics. No match or several matching profiles return a Diagnostic.
// Call validate_image_catalog first. The result points into the caller's catalog, which must remain alive and unchanged while used.
std::expected<const ImageProfile *, Diagnostic> recognize_image(std::string_view filename, const ImageLayout &layout,
                                                                std::span<const ImageProfile> catalog);

// Probes a supported basename. No loaded match returns an empty optional. Failures return a Diagnostic.
// Validate the catalog first and keep it alive and unchanged while the result is used; a match is pinned until process exit.
std::expected<std::optional<LoadedImage>, Diagnostic> probe_loaded_image(std::string_view filename, std::span<const ImageProfile> catalog);

// Inspects the actual process executable, independent of its basename. A recognized mapping is pinned until exit.
// Validate the catalog first and keep it alive and unchanged while the result is used.
std::expected<LoadedImage, Diagnostic> probe_main_image(std::span<const ImageProfile> catalog);

// Returns this runtime architecture's built-in profiles; the catalog and its referenced data have static lifetime.
std::span<const ImageProfile> known_image_profiles() noexcept;

} // namespace fc::runtime
