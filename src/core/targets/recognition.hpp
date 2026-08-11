#pragma once

#include <FusionCutter/target.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>

namespace fusioncutter::targets {

struct SectionProfile {
    std::string_view name;
    std::uint32_t virtual_address;
    std::uint32_t virtual_size;
};

struct ImageMarker {
    std::uint32_t rva;
    std::span<const std::byte> expected;
};

struct ImageProfile {
    std::string_view fingerprint;
    TargetLayout layout;
    TargetImage identity;
    std::string_view basename;
    Architecture architecture;
    std::uint32_t size_of_image;
    std::optional<std::uint32_t> timestamp;
    std::span<const SectionProfile> sections;
    std::optional<ImageMarker> marker;
};

struct RecognizedImage {
    TargetContext context;
    std::string_view fingerprint;
};

enum class RecognitionErrorKind {
    MalformedImage,
    UnknownImage,
    AmbiguousImage,
};

struct RecognitionError {
    RecognitionErrorKind kind;
    std::string_view detail;
};

[[nodiscard]] std::expected<RecognizedImage, RecognitionError>
recognize_mapped_image(std::string_view basename, HostRole role, std::uintptr_t base,
                       std::span<const std::byte> mapped_image, std::span<const ImageProfile> profiles);

} // namespace fusioncutter::targets
