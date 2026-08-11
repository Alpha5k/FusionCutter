#include <FusionCutter/settings.hpp>

#include <algorithm>
#include <utility>

namespace fusioncutter::settings_detail {

bool ascii_iequals(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }

    const auto ascii_lower = [](char value) {
        return value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A')) : value;
    };
    return std::ranges::equal(left, right, {}, ascii_lower, ascii_lower);
}

OutcomeReason value_error(std::string_view key, std::string message) {
    return {"setting '" + std::string(key) + "' " + std::move(message), std::nullopt, std::nullopt};
}

} // namespace fusioncutter::settings_detail

namespace fusioncutter {

std::optional<std::string_view> KeyedStrings::value(std::string_view key) const noexcept {
    const auto found = std::ranges::find_if(values_, [&](const auto& entry) {
        return settings_detail::ascii_iequals(entry.key, key);
    });
    if (found == values_.end()) {
        return std::nullopt;
    }
    return found->value;
}

void KeyedStrings::set(std::string_view key, std::string value) {
    const auto found = std::ranges::find_if(values_, [&](const auto& entry) {
        return settings_detail::ascii_iequals(entry.key, key);
    });
    if (found != values_.end()) {
        found->value = std::move(value);
        return;
    }
    values_.push_back({std::string(key), std::move(value)});
}

std::type_index SettingsDefinition::settings_type() const noexcept {
    return schema_ == nullptr ? std::type_index{typeid(NoSettings)} : schema_->settings_type();
}

std::span<const SettingMetadata> SettingsDefinition::metadata() const noexcept {
    return schema_ == nullptr ? std::span<const SettingMetadata>{} : schema_->metadata();
}

std::expected<void, OutcomeReason> SettingsDefinition::validate_metadata() const {
    return schema_ == nullptr ? std::expected<void, OutcomeReason>{} : schema_->validate_metadata();
}

ResolvedSettings SettingsDefinition::make_defaults() const {
    return schema_ == nullptr ? ResolvedSettings::make(NoSettings{}) : schema_->make_defaults();
}

std::optional<std::size_t> SettingsDefinition::find(std::string_view group, std::string_view key) const noexcept {
    const auto values = metadata();
    const auto found = std::ranges::find_if(values, [&](const auto& value) {
        return settings_detail::ascii_iequals(value.group, group) && settings_detail::ascii_iequals(value.key, key);
    });
    if (found == values.end()) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(found - values.begin());
}

std::expected<void, OutcomeReason> SettingsDefinition::apply(ResolvedSettings& settings, std::size_t index,
                                                             std::string_view value) const {
    if (schema_ == nullptr || settings.value_ == nullptr || settings.type_ != schema_->settings_type() ||
        index >= schema_->metadata().size()) {
        return std::unexpected(settings_detail::value_error({}, "settings application is invalid"));
    }
    return schema_->apply_value(settings.value_.get(), index, value);
}

std::expected<void, OutcomeReason> SettingsDefinition::validate(ResolvedSettings& settings) const {
    if (schema_ == nullptr) {
        return {};
    }
    if (settings.value_ == nullptr || settings.type_ != schema_->settings_type()) {
        return std::unexpected(settings_detail::value_error({}, "settings type does not match its schema"));
    }
    return schema_->validate_settings(settings.value_.get());
}

} // namespace fusioncutter
