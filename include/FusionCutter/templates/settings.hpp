#pragma once

#include "../settings.hpp"

#include <algorithm>
#include <cassert>
#include <charconv>
#include <cmath>
#include <limits>
#include <system_error>

namespace fusioncutter::settings_detail {

template <typename Value> [[nodiscard]] std::string format_value(const Value& value) {
    if constexpr (std::same_as<Value, bool>) {
        return value ? "1" : "0";
    } else if constexpr (std::integral<Value>) {
        char buffer[std::numeric_limits<Value>::digits10 + 4]{};
        const auto [end, error] = std::to_chars(std::begin(buffer), std::end(buffer), value);
        return error == std::errc{} ? std::string(buffer, end) : std::string{};
    } else if constexpr (std::floating_point<Value>) {
        char buffer[64]{};
        const auto [end, error] =
            std::to_chars(std::begin(buffer), std::end(buffer), value, std::chars_format::general);
        return error == std::errc{} ? std::string(buffer, end) : std::string{};
    } else {
        return value;
    }
}

template <typename Value> [[nodiscard]] std::optional<Value> parse_value(std::string_view input) {
    if constexpr (std::same_as<Value, bool>) {
        if (input == "1" || ascii_iequals(input, "true") || ascii_iequals(input, "on")) {
            return true;
        }
        if (input == "0" || ascii_iequals(input, "false") || ascii_iequals(input, "off")) {
            return false;
        }
        return std::nullopt;
    } else if constexpr (std::integral<Value>) {
        Value value{};
        const auto [end, error] = std::from_chars(input.data(), input.data() + input.size(), value, 10);
        if (error != std::errc{} || end != input.data() + input.size()) {
            return std::nullopt;
        }
        return value;
    } else if constexpr (std::floating_point<Value>) {
        Value value{};
        const auto [end, error] =
            std::from_chars(input.data(), input.data() + input.size(), value, std::chars_format::general);
        if (error != std::errc{} || end != input.data() + input.size() || !std::isfinite(value)) {
            return std::nullopt;
        }
        return value;
    } else {
        return std::string(input);
    }
}

class SettingsSchemaInterface {
  public:
    virtual ~SettingsSchemaInterface() = default;

    [[nodiscard]] virtual std::type_index settings_type() const noexcept = 0;
    [[nodiscard]] virtual std::span<const SettingMetadata> metadata() const noexcept = 0;
    [[nodiscard]] virtual std::expected<void, OutcomeReason> validate_metadata() const = 0;
    [[nodiscard]] virtual ResolvedSettings make_defaults() const = 0;
    [[nodiscard]] virtual std::expected<void, OutcomeReason> apply_value(void* settings, std::size_t index,
                                                                         std::string_view value) const = 0;
    [[nodiscard]] virtual std::expected<void, OutcomeReason> validate_settings(void* settings) const = 0;
};

template <typename Settings> class SettingsSchemaModel final : public SettingsSchemaInterface {
  public:
    explicit SettingsSchemaModel(SettingsSchema<Settings> schema) : validator_(schema.validate) {
        append_entries({}, std::move(schema.values));
        for (auto& group : schema.groups) {
            group_names_.push_back(group.name);
            append_entries(group.name, std::move(group.values));
        }
    }

    [[nodiscard]] std::type_index settings_type() const noexcept override {
        return typeid(Settings);
    }

    [[nodiscard]] std::span<const SettingMetadata> metadata() const noexcept override {
        return metadata_;
    }

    [[nodiscard]] std::expected<void, OutcomeReason> validate_metadata() const override {
        for (std::size_t index = 0; index < group_names_.size(); ++index) {
            if (group_names_[index].empty()) {
                return std::unexpected(value_error({}, "settings group name is empty"));
            }
            for (std::size_t other = index + 1; other < group_names_.size(); ++other) {
                if (ascii_iequals(group_names_[index], group_names_[other])) {
                    return std::unexpected(value_error({}, "settings group names are duplicated"));
                }
            }
        }

        for (std::size_t index = 0; index < entries_.size(); ++index) {
            const auto& entry = entries_[index];
            if (entry.metadata.key.empty()) {
                return std::unexpected(value_error({}, "setting key is empty"));
            }
            if (entry.metadata_error.has_value()) {
                return std::unexpected(value_error(entry.metadata.key, *entry.metadata_error));
            }
            for (std::size_t other = index + 1; other < entries_.size(); ++other) {
                if (ascii_iequals(entry.metadata.group, entries_[other].metadata.group) &&
                    ascii_iequals(entry.metadata.key, entries_[other].metadata.key)) {
                    return std::unexpected(value_error(entry.metadata.key, "is declared more than once"));
                }
            }
            for (std::size_t choice = 0; choice < entry.metadata.choices.size(); ++choice) {
                if (entry.metadata.choices[choice].empty()) {
                    return std::unexpected(value_error(entry.metadata.key, "has an empty choice name"));
                }
                for (std::size_t other = choice + 1; other < entry.metadata.choices.size(); ++other) {
                    if (ascii_iequals(entry.metadata.choices[choice], entry.metadata.choices[other])) {
                        return std::unexpected(value_error(entry.metadata.key, "has duplicate choice names"));
                    }
                }
            }
        }
        return {};
    }

    [[nodiscard]] ResolvedSettings make_defaults() const override {
        Settings settings{};
        for (const auto& entry : entries_) {
            entry.apply_default(settings);
        }
        return ResolvedSettings::make(std::move(settings));
    }

    [[nodiscard]] std::expected<void, OutcomeReason> apply_value(void* settings, std::size_t index,
                                                                 std::string_view value) const override {
        return entries_[index].apply_value(*static_cast<Settings*>(settings), value);
    }

    [[nodiscard]] std::expected<void, OutcomeReason> validate_settings(void* settings) const override {
        if (validator_ == nullptr) {
            return {};
        }
        return validator_(*static_cast<Settings*>(settings));
    }

  private:
    void append_entries(std::string_view group, std::vector<SettingEntry<Settings>> entries) {
        for (auto& entry : entries) {
            entry.metadata.group = group;
            metadata_.push_back(entry.metadata);
            entries_.push_back(std::move(entry));
        }
    }

    std::vector<SettingEntry<Settings>> entries_;
    std::vector<SettingMetadata> metadata_;
    std::vector<std::string> group_names_;
    SettingsValidator<Settings> validator_{};
};

} // namespace fusioncutter::settings_detail

namespace fusioncutter {

template <typename Settings> ResolvedSettings ResolvedSettings::make(Settings value) {
    ResolvedSettings result;
    result.value_ = std::unique_ptr<void, settings_detail::SettingsDeleter>{
        new Settings(std::move(value)), settings_detail::SettingsDeleter{[](void* stored) noexcept {
            delete static_cast<Settings*>(stored);
        }}};
    result.type_ = typeid(Settings);
    return result;
}

template <typename Settings> bool ResolvedSettings::is() const noexcept {
    return value_ != nullptr && type_ == typeid(Settings);
}

template <typename Settings> Settings ResolvedSettings::take() && {
    assert(is<Settings>());
    auto result = std::move(*static_cast<Settings*>(value_.get()));
    value_.reset();
    type_ = typeid(void);
    return result;
}

template <typename Settings> SettingsDefinition SettingsDefinition::from(SettingsSchema<Settings> schema) {
    SettingsDefinition result;
    result.schema_ = std::make_shared<settings_detail::SettingsSchemaModel<Settings>>(std::move(schema));
    return result;
}

template <typename Settings, settings_detail::ScalarSetting Member> class ScalarSettingBuilder {
  public:
    ScalarSettingBuilder(std::string_view key, Member Settings::* member, Member default_value)
        : key_(key), member_(member), default_(std::move(default_value)) {}

    ScalarSettingBuilder& description(std::string_view description) {
        description_ = description;
        return *this;
    }

    ScalarSettingBuilder& range(Member minimum, Member maximum)
        requires((std::integral<Member> && !std::same_as<Member, bool>) || std::floating_point<Member>)
    {
        minimum_ = minimum;
        maximum_ = maximum;
        return *this;
    }

    ScalarSettingBuilder& max_length(std::size_t maximum)
        requires std::same_as<Member, std::string>
    {
        maximum_length_ = maximum;
        return *this;
    }

    [[nodiscard]] operator SettingEntry<Settings>() {
        SettingEntry<Settings> entry;
        entry.metadata = {
            {}, std::string(key_), std::string(description_), kind(), settings_detail::format_value(default_), {},
        };
        entry.apply_default = [member = member_, value = default_](Settings& settings) {
            settings.*member = value;
        };
        entry.apply_value = [key = std::string(key_), member = member_, minimum = minimum_, maximum = maximum_,
                             maximum_length = maximum_length_](
                                Settings& settings, std::string_view input) -> std::expected<void, OutcomeReason> {
            auto parsed = settings_detail::parse_value<Member>(input);
            if (!parsed.has_value()) {
                return std::unexpected(settings_detail::value_error(key, "has an invalid value"));
            }
            if (minimum.has_value() && (*parsed < *minimum || *parsed > *maximum)) {
                return std::unexpected(settings_detail::value_error(key, "is outside its allowed range"));
            }
            if constexpr (std::same_as<Member, std::string>) {
                if (maximum_length.has_value() && parsed->size() > *maximum_length) {
                    return std::unexpected(settings_detail::value_error(key, "exceeds its maximum length"));
                }
            }
            settings.*member = std::move(*parsed);
            return {};
        };

        if (minimum_.has_value() && (*minimum_ > *maximum_ || default_ < *minimum_ || default_ > *maximum_)) {
            entry.metadata_error = "has an invalid range or default";
        }
        if constexpr (std::same_as<Member, std::string>) {
            if (maximum_length_.has_value() && default_.size() > *maximum_length_) {
                entry.metadata_error = "has a default that exceeds its maximum length";
            }
        }
        return entry;
    }

  private:
    [[nodiscard]] static constexpr SettingKind kind() noexcept {
        if constexpr (std::same_as<Member, bool>) {
            return SettingKind::Boolean;
        } else if constexpr (std::signed_integral<Member>) {
            return SettingKind::SignedInteger;
        } else if constexpr (std::unsigned_integral<Member>) {
            return SettingKind::UnsignedInteger;
        } else if constexpr (std::floating_point<Member>) {
            return SettingKind::FloatingPoint;
        } else {
            return SettingKind::String;
        }
    }

    std::string_view key_;
    Member Settings::* member_;
    Member default_;
    std::string_view description_;
    std::optional<Member> minimum_;
    std::optional<Member> maximum_;
    std::optional<std::size_t> maximum_length_;
};

template <typename Settings, settings_detail::ScalarSetting Member>
ScalarSettingBuilder<Settings, Member> setting(std::string_view key, Member Settings::* member, Member default_value) {
    return {key, member, std::move(default_value)};
}

template <typename Settings, typename Choice>
    requires std::is_enum_v<Choice>
class ChoiceSettingBuilder {
  public:
    ChoiceSettingBuilder(std::string_view key, Choice Settings::* member, Choice default_value,
                         std::initializer_list<ChoiceValue<Choice>> choices)
        : key_(key), member_(member), default_(default_value), choices_(choices) {}

    ChoiceSettingBuilder& description(std::string_view description) {
        description_ = description;
        return *this;
    }

    [[nodiscard]] operator SettingEntry<Settings>() {
        SettingEntry<Settings> entry;
        entry.metadata = {{}, std::string(key_), std::string(description_), SettingKind::Choice, {}, {}};
        for (const auto& choice_value : choices_) {
            entry.metadata.choices.emplace_back(choice_value.name);
            if (choice_value.value == default_) {
                entry.metadata.default_value = choice_value.name;
            }
        }
        if (entry.metadata.default_value.empty()) {
            entry.metadata_error = "has a default that is not one of its choices";
        }

        entry.apply_default = [member = member_, value = default_](Settings& settings) {
            settings.*member = value;
        };
        entry.apply_value = [key = std::string(key_), member = member_, choices = choices_](
                                Settings& settings, std::string_view input) -> std::expected<void, OutcomeReason> {
            const auto found = std::ranges::find_if(choices, [&](const auto& choice_value) {
                return settings_detail::ascii_iequals(choice_value.name, input);
            });
            if (found == choices.end()) {
                return std::unexpected(settings_detail::value_error(key, "is not one of its accepted choices"));
            }
            settings.*member = found->value;
            return {};
        };
        return entry;
    }

  private:
    std::string_view key_;
    Choice Settings::* member_;
    Choice default_;
    std::string_view description_;
    std::vector<ChoiceValue<Choice>> choices_;
};

template <typename Settings, typename Choice>
    requires std::is_enum_v<Choice>
ChoiceSettingBuilder<Settings, Choice> choice(std::string_view key, Choice Settings::* member, Choice default_value,
                                              std::initializer_list<ChoiceValue<Choice>> choices) {
    return {key, member, default_value, choices};
}

template <typename Settings>
SettingsGroup<Settings> keyed_string_group(std::string_view name, KeyedStrings Settings::* member,
                                           std::initializer_list<KeyedStringSetting> values) {
    SettingsGroup<Settings> group{std::string(name), {}};
    group.values.reserve(values.size());
    for (const auto& value : values) {
        SettingEntry<Settings> entry;
        entry.metadata = {{},
                          std::string(value.key),
                          std::string(value.description),
                          SettingKind::String,
                          std::string(value.default_value),
                          {}};
        if (value.maximum_length != 0 && value.default_value.size() > value.maximum_length) {
            entry.metadata_error = "has a default that exceeds its maximum length";
        }
        entry.apply_default = [member, key = std::string(value.key),
                               default_value = std::string(value.default_value)](Settings& settings) {
            (settings.*member).set(key, default_value);
        };
        entry.apply_value = [member, key = std::string(value.key), maximum_length = value.maximum_length](
                                Settings& settings, std::string_view input) -> std::expected<void, OutcomeReason> {
            if (maximum_length != 0 && input.size() > maximum_length) {
                return std::unexpected(settings_detail::value_error(key, "exceeds its maximum length"));
            }
            (settings.*member).set(key, std::string(input));
            return {};
        };
        group.values.push_back(std::move(entry));
    }
    return group;
}

template <typename Settings>
SettingsGroup<Settings> settings_group(std::string_view name, std::initializer_list<SettingEntry<Settings>> values) {
    return {std::string(name), values};
}

} // namespace fusioncutter
