#pragma once

#include "outcome.hpp"
#include "settings.hpp"

#include <array>
#include <cstddef>
#include <expected>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>

namespace fusioncutter {

namespace environment_detail {
[[nodiscard]] std::expected<std::optional<std::string>, OutcomeReason> read_variable(std::string_view name);
} // namespace environment_detail

template <settings_detail::ScalarSetting Value>
[[nodiscard]] std::expected<std::optional<Value>, OutcomeReason> read_environment_value(std::string_view name);

template <typename Choice>
    requires std::is_enum_v<Choice>
[[nodiscard]] std::expected<std::optional<Choice>, OutcomeReason>
read_environment_choice(std::string_view name, std::span<const ChoiceValue<Choice>> choices);

template <typename Choice, std::size_t Size>
    requires std::is_enum_v<Choice>
[[nodiscard]] std::expected<std::optional<Choice>, OutcomeReason>
read_environment_choice(std::string_view name, const std::array<ChoiceValue<Choice>, Size>& choices);

template <typename Choice>
    requires std::is_enum_v<Choice>
[[nodiscard]] std::expected<std::optional<Choice>, OutcomeReason>
read_environment_choice(std::string_view name, std::initializer_list<ChoiceValue<Choice>> choices);

} // namespace fusioncutter

#include "templates/environment.hpp"
