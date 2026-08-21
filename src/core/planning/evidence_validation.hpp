#pragma once

#include "planning_types.hpp"

#include "../targets/image_view.hpp"

#include <expected>
#include <span>
#include <string>

namespace fc::planning {

// Returns the image extent observed by one normalized evidence record for claims and later revalidation.
[[nodiscard]] std::uint64_t evidence_extent(const EvidenceRecord& evidence, FC_Architecture architecture) noexcept;

// Compares normalized evidence through the image view during plan validation and both bounded revalidation passes.
[[nodiscard]] std::expected<void, std::string> validate_location_evidence(const targets::ImageView& image,
                                                                          FC_Architecture architecture,
                                                                          const LocationRecord& location);

// Installed hook preimages replace only their overwritten bytes when late work validates a logical original view.
[[nodiscard]] std::expected<void, std::string>
validate_location_evidence(const targets::ImageView& image, FC_Architecture architecture,
                           const LocationRecord& location, FC_TargetImage image_id,
                           std::span<const InstalledHookSite> installed_hooks);

// Rechecks a direct call or jump and returns its decoded target without requiring that target to remain in the image.
[[nodiscard]] std::expected<std::uintptr_t, std::string> validate_direct_branch(const targets::ImageView& image,
                                                                                FC_Architecture architecture,
                                                                                const LocationRecord& location,
                                                                                FC_RedirectKind kind);

} // namespace fc::planning
