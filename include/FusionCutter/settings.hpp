#pragma once

#include "outcome.hpp"

#include <array>
#include <concepts>
#include <cstddef>
#include <expected>
#include <functional>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <typeindex>
#include <type_traits>
#include <utility>
#include <vector>

namespace fusioncutter {

enum class SettingKind {
    Boolean,
    SignedInteger,
    UnsignedInteger,
    FloatingPoint,
    String,
    Choice,
};

struct SettingMetadata {
    std::string group;
    std::string key;
    std::string description;
    SettingKind kind;
    std::string default_value;
    std::vector<std::string> choices;
};

struct KeyedStringValue {
    std::string key;
    std::string value;
};

struct KeyedStringSetting {
    std::string_view key;
    std::string_view default_value;
    std::string_view description{};
    std::size_t maximum_length{};
};

template <typename Settings> struct SettingEntry {
    SettingMetadata metadata;
    std::function<void(Settings&)> apply_default;
    std::function<std::expected<void, OutcomeReason>(Settings&, std::string_view)> apply_value;
    std::optional<std::string> metadata_error;
};

template <typename Settings> struct SettingsGroup {
    std::string name;
    std::vector<SettingEntry<Settings>> values;
};

template <typename Settings> using SettingsValidator = std::expected<void, OutcomeReason> (*)(Settings&);

template <typename Settings> struct SettingsSchema {
    std::vector<SettingEntry<Settings>> values;
    std::vector<SettingsGroup<Settings>> groups;
    SettingsValidator<Settings> validate{};
};

class KeyedStrings;

template <typename Settings>
[[nodiscard]] SettingsGroup<Settings> keyed_string_group(std::string_view name, KeyedStrings Settings::* member,
                                                         std::span<const KeyedStringSetting> values);

template <typename Settings, std::size_t Size>
[[nodiscard]] SettingsGroup<Settings> keyed_string_group(std::string_view name, KeyedStrings Settings::* member,
                                                         const std::array<KeyedStringSetting, Size>& values);

template <typename Settings>
[[nodiscard]] SettingsGroup<Settings> keyed_string_group(std::string_view name, KeyedStrings Settings::* member,
                                                         std::initializer_list<KeyedStringSetting> values);

class KeyedStrings {
  public:
    [[nodiscard]] std::optional<std::string_view> value(std::string_view key) const noexcept;
    [[nodiscard]] std::span<const KeyedStringValue> values() const noexcept {
        return values_;
    }

  private:
    template <typename Settings>
    friend SettingsGroup<Settings> keyed_string_group(std::string_view name, KeyedStrings Settings::* member,
                                                      std::span<const KeyedStringSetting> values);

    void set(std::string_view key, std::string value);

    std::vector<KeyedStringValue> values_;
};

namespace settings_detail {

template <typename Value>
inline constexpr bool kCharacterType =
    std::same_as<std::remove_cv_t<Value>, char> || std::same_as<std::remove_cv_t<Value>, signed char> ||
    std::same_as<std::remove_cv_t<Value>, unsigned char> || std::same_as<std::remove_cv_t<Value>, wchar_t> ||
    std::same_as<std::remove_cv_t<Value>, char8_t> || std::same_as<std::remove_cv_t<Value>, char16_t> ||
    std::same_as<std::remove_cv_t<Value>, char32_t>;

template <typename Value>
concept ScalarSetting =
    std::same_as<std::remove_cv_t<Value>, bool> || (std::integral<Value> && !kCharacterType<Value>) ||
    std::floating_point<Value> || std::same_as<std::remove_cv_t<Value>, std::string>;

struct SettingsDeleter {
    void (*destroy)(void*) noexcept {};

    void operator()(void* value) const noexcept {
        if (destroy != nullptr) {
            destroy(value);
        }
    }
};

class SettingsSchemaInterface;
template <typename Settings> class SettingsSchemaModel;

[[nodiscard]] bool ascii_iequals(std::string_view left, std::string_view right) noexcept;
[[nodiscard]] OutcomeReason value_error(std::string_view key, std::string message);

} // namespace settings_detail

class ResolvedSettings {
  public:
    ResolvedSettings() = default;
    ResolvedSettings(const ResolvedSettings&) = delete;
    ResolvedSettings& operator=(const ResolvedSettings&) = delete;
    ResolvedSettings(ResolvedSettings&&) noexcept = default;
    ResolvedSettings& operator=(ResolvedSettings&&) noexcept = default;
    ~ResolvedSettings() = default;

    template <typename Settings> [[nodiscard]] static ResolvedSettings make(Settings value);
    template <typename Settings> [[nodiscard]] bool is() const noexcept;
    template <typename Settings> [[nodiscard]] Settings take() &&;

  private:
    friend class SettingsDefinition;

    std::unique_ptr<void, settings_detail::SettingsDeleter> value_;
    std::type_index type_{typeid(void)};
};

struct NoSettings {};

class SettingsDefinition {
  public:
    SettingsDefinition() = default;

    template <typename Settings> [[nodiscard]] static SettingsDefinition from(SettingsSchema<Settings> schema);

    [[nodiscard]] std::type_index settings_type() const noexcept;
    [[nodiscard]] std::span<const SettingMetadata> metadata() const noexcept;
    [[nodiscard]] std::expected<void, OutcomeReason> validate_metadata() const;
    [[nodiscard]] ResolvedSettings make_defaults() const;
    [[nodiscard]] std::optional<std::size_t> find(std::string_view group, std::string_view key) const noexcept;
    [[nodiscard]] std::expected<void, OutcomeReason> apply(ResolvedSettings& settings, std::size_t index,
                                                           std::string_view value) const;
    [[nodiscard]] std::expected<void, OutcomeReason> validate(ResolvedSettings& settings) const;

  private:
    std::shared_ptr<const settings_detail::SettingsSchemaInterface> schema_;
};

template <typename Settings, settings_detail::ScalarSetting Member> class ScalarSettingBuilder;

template <typename Settings, settings_detail::ScalarSetting Member>
[[nodiscard]] ScalarSettingBuilder<Settings, Member> setting(std::string_view key, Member Settings::* member,
                                                             Member default_value);

template <typename Choice> struct ChoiceValue {
    std::string_view name;
    Choice value;
};

template <typename Settings, typename Choice>
    requires std::is_enum_v<Choice>
class ChoiceSettingBuilder;

template <typename Settings, typename Choice>
    requires std::is_enum_v<Choice>
[[nodiscard]] ChoiceSettingBuilder<Settings, Choice> choice(std::string_view key, Choice Settings::* member,
                                                            Choice default_value,
                                                            std::span<const ChoiceValue<Choice>> choices);

template <typename Settings, typename Choice, std::size_t Size>
    requires std::is_enum_v<Choice>
[[nodiscard]] ChoiceSettingBuilder<Settings, Choice> choice(std::string_view key, Choice Settings::* member,
                                                            Choice default_value,
                                                            const std::array<ChoiceValue<Choice>, Size>& choices);

template <typename Settings, typename Choice>
    requires std::is_enum_v<Choice>
[[nodiscard]] ChoiceSettingBuilder<Settings, Choice> choice(std::string_view key, Choice Settings::* member,
                                                            Choice default_value,
                                                            std::initializer_list<ChoiceValue<Choice>> choices);

template <typename Choice>
    requires std::is_enum_v<Choice>
[[nodiscard]] constexpr std::string_view choice_name(Choice value,
                                                     std::span<const ChoiceValue<Choice>> choices) noexcept;

template <typename Choice, std::size_t Size>
    requires std::is_enum_v<Choice>
[[nodiscard]] constexpr std::string_view choice_name(Choice value,
                                                     const std::array<ChoiceValue<Choice>, Size>& choices) noexcept;

template <typename Choice>
    requires std::is_enum_v<Choice>
[[nodiscard]] constexpr std::string_view choice_name(Choice value,
                                                     std::initializer_list<ChoiceValue<Choice>> choices) noexcept;

template <typename Settings>
[[nodiscard]] SettingsGroup<Settings> settings_group(std::string_view name,
                                                     std::initializer_list<SettingEntry<Settings>> values);

} // namespace fusioncutter

#include "templates/settings.hpp"
