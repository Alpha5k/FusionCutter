#pragma once

#include "outcome.hpp"
#include "settings.hpp"

#include <array>
#include <cstddef>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>

namespace fusioncutter {

[[nodiscard]] std::expected<std::optional<std::string>, OutcomeReason> read_environment_variable(std::string_view name);

template <settings_detail::ScalarSetting Value>
[[nodiscard]] std::expected<std::optional<Value>, OutcomeReason> read_environment_value(std::string_view name);

template <typename Choice, std::size_t Size>
    requires std::is_enum_v<Choice>
[[nodiscard]] std::expected<std::optional<Choice>, OutcomeReason>
read_environment_choice(std::string_view name, const std::array<ChoiceValue<Choice>, Size>& choices);

} // namespace fusioncutter

#include "templates/environment.hpp"
