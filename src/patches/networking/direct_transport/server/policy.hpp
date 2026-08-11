#pragma once

#include "../shared/protocol.hpp"

#include <FusionCutter/environment.hpp>
#include <FusionCutter/settings.hpp>

#include <array>
#include <expected>
#include <utility>

namespace fusioncutter::patches::direct_transport::server {

inline constexpr std::array kPolicyChoices{
    ChoiceValue{"Disabled", Policy::Disabled},
    ChoiceValue{"PreferDirect", Policy::PreferDirect},
    ChoiceValue{"RequireDirectPatched", Policy::RequireDirectPatched},
    ChoiceValue{"RequireDirectAll", Policy::RequireDirectAll},
};

// Applies BF2_DIRECT_POLICY when present, otherwise preserving the configured server policy.
[[nodiscard]] inline std::expected<Policy, OutcomeReason> resolve_policy(Policy configured) {
    auto override = read_environment_choice("BF2_DIRECT_POLICY", kPolicyChoices);
    if (!override.has_value()) {
        return std::unexpected(std::move(override.error()));
    }
    return override->value_or(configured);
}

[[nodiscard]] inline constexpr std::string_view policy_name(Policy policy) noexcept {
    const auto name = choice_name(policy, kPolicyChoices);
    return name.empty() ? "Unknown" : name;
}

} // namespace fusioncutter::patches::direct_transport::server
