#pragma once

#include <FusionCutter/SDK.hpp>

#include <string_view>

namespace fc::fixtures::relocated_plan {

inline constexpr std::string_view kPluginId = "RelocatedPlanSlice";
inline constexpr std::string_view kStoragePatchId = "RelocatedStorage";
inline constexpr std::string_view kConflictAPatchId = "RelocatedConflictA";
inline constexpr std::string_view kConflictBPatchId = "RelocatedConflictB";
inline constexpr std::string_view kIndependentPatchId = "RelocatedIndependent";
inline constexpr std::string_view kGroupId = "RelocatedFeatures";

// One semantic layout names the reviewed addresses consumed by every patch in this fixture plugin.
struct Layout {
    fc::Rva pointer_slot;
    fc::Rva relative_slot;
    fc::Rva call_site;
    fc::Rva function;
    fc::Rva code;
    fc::Rva vtable;
    fc::Rva mutable_data;
    fc::Rva conflict;
    fc::Rva independent;
};

// Maps each supported target to reviewed addresses so the plan can use semantic names instead of scattered RVAs.
[[nodiscard]] constexpr Layout layout_for(fc::TargetInfo target) noexcept {
    if (target.layout == fc::TargetLayout::SteamRetail) {
        return {{0x3de100}, {0x3de110}, {0x1200}, {0x1300}, {0x1400}, {0x36b100}, {0x3de200}, {0x3de300}, {0x3de400}};
    }
    return {{0x639100}, {0x639110}, {0x1200}, {0x1300}, {0x1400}, {0x501100}, {0x639200}, {0x639300}, {0x639400}};
}

// Builds the contribution containing all patches used by the public testing API and verifier tests.
[[nodiscard]] fc::Plugin build_plugin();

} // namespace fc::fixtures::relocated_plan
