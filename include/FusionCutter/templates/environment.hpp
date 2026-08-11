#pragma once

#include "../environment.hpp"

#include <algorithm>
#include <concepts>
#include <string>
#include <string_view>
#include <utility>

namespace fusioncutter {
namespace environment_detail {

[[nodiscard]] inline OutcomeReason invalid_value(std::string_view name, std::string expected) {
    return {"environment variable '" + std::string(name) + "' has an invalid value; expected " + std::move(expected),
            "Read environment variable",
            {}};
}

template <typename Value> [[nodiscard]] constexpr std::string_view expected_value() noexcept {
    if constexpr (std::same_as<Value, bool>) {
        return "0/1, true/false, or off/on";
    } else if constexpr (std::integral<Value>) {
        return "an integer";
    } else if constexpr (std::floating_point<Value>) {
        return "a finite number";
    } else {
        return "text";
    }
}

} // namespace environment_detail

template <settings_detail::ScalarSetting Value>
std::expected<std::optional<Value>, OutcomeReason> read_environment_value(std::string_view name) {
    auto value = read_environment_variable(name);
    if (!value.has_value()) {
        return std::unexpected(std::move(value.error()));
    }
    if (!value->has_value()) {
        return std::optional<Value>{};
    }

    auto parsed = settings_detail::parse_value<Value>(**value);
    if (!parsed.has_value()) {
        return std::unexpected(
            environment_detail::invalid_value(name, std::string(environment_detail::expected_value<Value>())));
    }
    return std::optional<Value>{std::move(*parsed)};
}

template <typename Choice, std::size_t Size>
    requires std::is_enum_v<Choice>
std::expected<std::optional<Choice>, OutcomeReason>
read_environment_choice(std::string_view name, const std::array<ChoiceValue<Choice>, Size>& choices) {
    static_assert(Size != 0);
    auto value = read_environment_variable(name);
    if (!value.has_value()) {
        return std::unexpected(std::move(value.error()));
    }
    if (!value->has_value()) {
        return std::optional<Choice>{};
    }

    const auto found = std::ranges::find_if(choices, [&](const auto& choice) {
        return settings_detail::ascii_iequals(choice.name, **value);
    });
    if (found != choices.end()) {
        return std::optional<Choice>{found->value};
    }

    std::string accepted;
    for (const auto& choice : choices) {
        if (!accepted.empty()) {
            accepted += ", ";
        }
        accepted += choice.name;
    }
    return std::unexpected(environment_detail::invalid_value(name, std::move(accepted)));
}

} // namespace fusioncutter
