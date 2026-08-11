#pragma once

#include "mapped_image.hpp"

#include "catalog/catalog.hpp"

#include <FusionCutter/target.hpp>

#include <cstddef>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace fusioncutter::verify {

struct VerifiedPlan {
    PatchId patch_id;
    HostRole role;
    std::size_t critical_dependencies;
};

struct VerifiedImage {
    TargetLayout layout;
    TargetImage identity;
    std::string_view fingerprint;
    std::vector<VerifiedPlan> plans;
};

[[nodiscard]] std::expected<VerifiedImage, std::string>
verify_supported_image(const std::filesystem::path& path, MappedImage& image, const catalog::Catalog& catalog);

} // namespace fusioncutter::verify
