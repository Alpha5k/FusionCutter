#pragma once

#include "image_view.hpp"
#include "target_profiles.hpp"

#include <array>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fc::targets {

// Error kinds let loaders distinguish unsupported binaries from malformed images and host failures.
enum class RecognitionErrorKind {
    Unsupported,
    Ambiguous,
    MalformedImage,
    System,
};

// Carries a user-facing cause and the recognition operation that produced it.
struct RecognitionError {
    RecognitionErrorKind kind{};
    std::string message;
    std::string operation;
};

// Permanent rejection while probing a late image identifies the operation that terminalizes every waiting patch.
struct LateProbeError {
    std::string message;
    std::string operation;
};

// nullopt means the reviewed module is still absent; an OwnedImage already holds the module reference on success.
using LateProbeResult = std::expected<std::optional<OwnedImage>, LateProbeError>;

// Returning target facts by value avoids exposing ownership of the recognized image set.
struct TargetInfo {
    FC_TargetLayout layout{};
    FC_HostRole role{};
    FC_Architecture architecture{};
    std::string_view image_profile;
};

// Target recognition returns the matched process-lifetime profile and the access policy derived from its PE image.
struct RecognizedImageFacts {
    const ImageProfile* profile{};
    std::vector<std::pair<std::uint32_t, std::uint32_t>> writable_ranges;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> readable_ranges;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> executable_ranges;
};

// Stateless recognition is shared by live startup and offline verifier mappings.
[[nodiscard]] std::expected<RecognizedImageFacts, RecognitionError>
recognize_mapped_image(std::string_view basename, std::span<const std::byte> mapped_image,
                       std::span<const ImageProfile> profiles = known_image_profiles());

// Takes ownership of an offline mapped image only after its complete fingerprint is recognized.
[[nodiscard]] std::expected<OwnedImage, RecognitionError>
recognize_owned_mapped_image(std::string_view basename, std::vector<std::byte> mapped_image,
                             std::span<const ImageProfile> profiles = known_image_profiles());

// Scenario hosts trust an explicitly selected canonical profile but still require a structurally valid mapped PE and
// derive all read/write/execute policy from that image's bounded section table.
[[nodiscard]] std::expected<OwnedImage, RecognitionError>
validate_synthetic_mapped_image(const ImageProfile& profile, std::vector<std::byte> mapped_image);

// Owns the stable slots for physical images and the one layout, role, and architecture shared by all of them.
class RecognizedTarget {
  public:
    RecognizedTarget(const RecognizedTarget&) = delete;
    RecognizedTarget& operator=(const RecognizedTarget&) = delete;
    RecognizedTarget(RecognizedTarget&&) noexcept = default;
    RecognizedTarget& operator=(RecognizedTarget&&) noexcept = default;

    // Target facts remain valid for the lifetime of this object; image lookup never transfers ownership.
    [[nodiscard]] FC_TargetLayout layout() const noexcept;
    [[nodiscard]] FC_HostRole role() const noexcept;
    [[nodiscard]] FC_Architecture architecture() const noexcept;
    // info() requires a present physical image; find() is the checked optional lookup.
    [[nodiscard]] TargetInfo info(FC_TargetImage image) const noexcept;
    [[nodiscard]] const ImageView* find(FC_TargetImage image) const noexcept;

    // A late image is installed once into its stable slot for that physical image.
    [[nodiscard]] bool add_late_image(OwnedImage image) noexcept;

    // Startup requires one reviewed Game image, except Classic requires its Bootstrap and Game pair.
    [[nodiscard]] static std::expected<RecognizedTarget, RecognitionError>
    create(FC_HostRole role, std::vector<OwnedImage> startup_images);

  private:
    RecognizedTarget(FC_TargetLayout layout, FC_HostRole role, FC_Architecture architecture) noexcept;

    static constexpr std::size_t kTargetImageSlotCount = 3;
    [[nodiscard]] static std::optional<std::size_t> slot(FC_TargetImage image) noexcept;

    FC_TargetLayout layout_{};
    FC_HostRole role_{};
    FC_Architecture architecture_{};
    std::array<std::optional<OwnedImage>, kTargetImageSlotCount> images_;
};

// The role is supplied by the loader entry that already distinguished client from dedicated server startup.
[[nodiscard]] std::expected<RecognizedTarget, RecognitionError> recognize_target(FC_HostRole role);

// Probes one framework-reviewed polled image without loading it or exposing an unpinned live view.
[[nodiscard]] LateProbeResult probe_late_image(const RecognizedTarget& target, FC_TargetImage image);

} // namespace fc::targets
