#pragma once

#include <FusionCutter/Abi.h>

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace fc::targets {

// One exact section fact used as part of an image fingerprint.
struct SectionProfile {
    std::string_view name;
    std::uint32_t virtual_address;
    std::uint32_t virtual_size;
};

// Limits a polled image to native publication mechanisms that remain coherent while its code may already execute.
enum class LateMutationPolicy {
    SuspendedHooksOnly,
};

// Describes the module visible to the loader and mutation proof owned by one approved profile for a late image.
struct LateImagePolicy {
    const wchar_t* module_name;
    LateMutationPolicy mutation;
};

// A profile holds reviewed PE facts and policy for late delivery; evidence remains metadata owned by its patch.
struct ImageProfile {
    std::string_view id;
    std::span<const std::string_view> basenames;
    FC_TargetLayout layout;
    FC_TargetImage image;
    FC_Architecture architecture;
    std::optional<LateImagePolicy> late;
    std::uint32_t size_of_image;
    std::optional<std::uint32_t> timestamp;
    std::span<const SectionProfile> sections;
};

// Returns the process-lifetime reviewed profile catalog in stable declaration order.
[[nodiscard]] std::span<const ImageProfile> known_image_profiles() noexcept;

// Resolves a profile ID case-insensitively and returns the process-lifetime record with its canonical spelling.
[[nodiscard]] const ImageProfile* find_image_profile(std::string_view id) noexcept;

// Reports whether a generation 1 tuple is a supported patch target.
[[nodiscard]] bool valid_target_tuple(FC_TargetLayout layout, FC_HostRole role, FC_TargetImage image) noexcept;

// Restricts recognized targets to deployment roles that exist for their reviewed layout.
[[nodiscard]] bool valid_target_role(FC_TargetLayout layout, FC_HostRole role) noexcept;

} // namespace fc::targets
