#include "recognition.hpp"

#include "pe_image.hpp"

#include <algorithm>
#include <limits>

namespace fusioncutter::targets {
namespace {

[[nodiscard]] constexpr char ascii_lower(char value) noexcept {
    if (value >= 'A' && value <= 'Z') {
        return static_cast<char>(value + ('a' - 'A'));
    }
    return value;
}

[[nodiscard]] bool basename_equal(std::string_view left, std::string_view right) noexcept {
    return left.size() == right.size() && std::ranges::equal(left, right, [](char left_value, char right_value) {
               return ascii_lower(left_value) == ascii_lower(right_value);
           });
}

[[nodiscard]] std::string_view section_name(const PeSectionFacts& section) noexcept {
    const auto terminator = std::ranges::find(section.name, '\0');
    return {section.name.data(), static_cast<std::size_t>(terminator - section.name.begin())};
}

[[nodiscard]] bool section_matches(const PeImageFacts& facts, const SectionProfile& profile) noexcept {
    if (profile.name.empty() || profile.name.size() > PeSectionFacts{}.name.size()) {
        return false;
    }

    return std::ranges::count_if(facts.sections, [&](const auto& section) {
               return section_name(section) == profile.name && section.virtual_address == profile.virtual_address &&
                      section.virtual_size == profile.virtual_size;
           }) == 1;
}

[[nodiscard]] bool marker_matches(std::span<const std::byte> image, const ImageMarker& marker) noexcept {
    const auto offset = static_cast<std::size_t>(marker.rva);
    if (marker.expected.empty() || offset > image.size() || marker.expected.size() > image.size() - offset) {
        return false;
    }

    return std::ranges::equal(marker.expected, image.subspan(offset, marker.expected.size()));
}

[[nodiscard]] bool profile_matches(std::string_view basename, std::span<const std::byte> mapped_image,
                                   const PeImageFacts& facts, const ImageProfile& profile) noexcept {
    if (profile.fingerprint.empty() || !basename_equal(basename, profile.basename) ||
        facts.architecture != profile.architecture || facts.size_of_image != profile.size_of_image ||
        (profile.timestamp.has_value() && facts.timestamp != *profile.timestamp)) {
        return false;
    }

    if (!std::ranges::all_of(profile.sections, [&facts](const SectionProfile& section) {
            return section_matches(facts, section);
        })) {
        return false;
    }

    return !profile.marker.has_value() || marker_matches(mapped_image.first(facts.size_of_image), *profile.marker);
}

} // namespace

std::expected<RecognizedImage, RecognitionError> recognize_mapped_image(std::string_view basename, HostRole role,
                                                                        std::uintptr_t base,
                                                                        std::span<const std::byte> mapped_image,
                                                                        std::span<const ImageProfile> profiles) {
    const auto facts = inspect_mapped_pe(mapped_image);
    if (!facts.has_value()) {
        return std::unexpected(RecognitionError{RecognitionErrorKind::MalformedImage, facts.error()});
    }

    if (base == 0 || base > std::numeric_limits<std::uintptr_t>::max() - facts->size_of_image) {
        return std::unexpected(
            RecognitionError{RecognitionErrorKind::MalformedImage, "mapped image base and size overflow"});
    }

    const ImageProfile* match = nullptr;
    for (const auto& profile : profiles) {
        if (!profile_matches(basename, mapped_image, *facts, profile)) {
            continue;
        }

        if (match != nullptr) {
            return std::unexpected(
                RecognitionError{RecognitionErrorKind::AmbiguousImage, "mapped image matches multiple profiles"});
        }
        match = &profile;
    }

    if (match == nullptr) {
        return std::unexpected(
            RecognitionError{RecognitionErrorKind::UnknownImage, "mapped image does not match a known profile"});
    }

    return RecognizedImage{
        TargetContext{match->layout, role,
                      ImageContext{match->identity, facts->architecture, base, facts->size_of_image}},
        match->fingerprint,
    };
}

} // namespace fusioncutter::targets
