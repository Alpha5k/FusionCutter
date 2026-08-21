#include "configuration_types.hpp"

#include "../catalog/definition_copy.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <ranges>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>

namespace fc::config {
namespace {

// Scalar parsers require complete consumption; whitespace and alternate bases are never normalized implicitly.
template <class Value> [[nodiscard]] std::optional<Value> parse_integer(std::string_view text) noexcept {
    Value value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value, 10);
    if (error != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

template <class Value> [[nodiscard]] bool within_range(Value value, const catalog::SettingDefinitionRecord& setting) {
    if (setting.has_range == FC_FALSE) {
        return true;
    }
    if constexpr (std::is_signed_v<Value>) {
        return value >= static_cast<Value>(setting.minimum.signed_value) &&
               value <= static_cast<Value>(setting.maximum.signed_value);
    } else if constexpr (std::is_unsigned_v<Value>) {
        return value >= static_cast<Value>(setting.minimum.unsigned_value) &&
               value <= static_cast<Value>(setting.maximum.unsigned_value);
    } else {
        return value >= static_cast<Value>(setting.minimum.floating_value) &&
               value <= static_cast<Value>(setting.maximum.floating_value);
    }
}

template <class Value>
[[nodiscard]] std::expected<ResolvedSettingValue, std::string>
resolve_signed(const catalog::SettingDefinitionRecord& setting, std::string_view text) {
    // Parsing into the declared width rejects transport values that would narrow before range comparison.
    const auto parsed = parse_integer<Value>(text);
    if (!parsed || !within_range(*parsed, setting)) {
        return std::unexpected("expected a representable signed integer within the declared range");
    }
    FC_SettingValue value{};
    value.signed_value = *parsed;
    return ResolvedSettingValue::scalar(setting.type, value);
}

template <class Value>
[[nodiscard]] std::expected<ResolvedSettingValue, std::string>
resolve_unsigned(const catalog::SettingDefinitionRecord& setting, std::string_view text) {
    // A leading sign is rejected by unsigned from_chars rather than being normalized into another domain.
    const auto parsed = parse_integer<Value>(text);
    if (!parsed || !within_range(*parsed, setting)) {
        return std::unexpected("expected a representable unsigned integer within the declared range");
    }
    FC_SettingValue value{};
    value.unsigned_value = *parsed;
    return ResolvedSettingValue::scalar(setting.type, value);
}

template <class Value>
[[nodiscard]] std::expected<ResolvedSettingValue, std::string>
resolve_floating(const catalog::SettingDefinitionRecord& setting, std::string_view text) {
    // Float32 rounds here and that rounded finite value remains authoritative for bounds and ABI delivery.
    Value value{};
    const auto [end, error] =
        std::from_chars(text.data(), text.data() + text.size(), value, std::chars_format::general);
    if (error != std::errc{} || end != text.data() + text.size() || !std::isfinite(value) ||
        !within_range(value, setting)) {
        return std::unexpected("expected a finite floating-point value within the declared range");
    }
    FC_SettingValue resolved{};
    resolved.floating_value = static_cast<double>(value);
    return ResolvedSettingValue::scalar(setting.type, resolved);
}

// Generated defaults use enough precision to reproduce the same accepted value on the next configuration read.
template <class Value> [[nodiscard]] std::string format_number(Value value) {
    char buffer[128]{};
    std::to_chars_result converted;
    if constexpr (std::is_floating_point_v<Value>) {
        converted = std::to_chars(buffer, std::end(buffer), value, std::chars_format::general,
                                  std::numeric_limits<Value>::max_digits10);
    } else {
        converted = std::to_chars(buffer, std::end(buffer), value);
    }
    return converted.ec == std::errc{} ? std::string(buffer, converted.ptr) : std::string{};
}

} // namespace

FC_SettingType ResolvedSettingValue::type() const noexcept {
    return type_;
}

FC_SettingValue ResolvedSettingValue::native_value() const noexcept {
    auto result = value_;
    if (type_ == FC_SETTING_STRING) {
        result.string_value = {string_.data(), static_cast<std::uint32_t>(string_.size())};
    }
    return result;
}

ResolvedSettingValue ResolvedSettingValue::scalar(FC_SettingType type, FC_SettingValue value) noexcept {
    ResolvedSettingValue result;
    result.type_ = type;
    result.value_ = value;
    return result;
}

ResolvedSettingValue ResolvedSettingValue::string(std::string value) {
    ResolvedSettingValue result;
    result.type_ = FC_SETTING_STRING;
    result.string_ = std::move(value);
    return result;
}

std::expected<ResolvedSettingValue, std::string> resolve_setting_value(const catalog::SettingDefinitionRecord& setting,
                                                                       std::string_view text) {
    // Boolean aliases are the only accepted noncanonical spellings; every numeric kind dispatches at declared width.
    if (setting.type == FC_SETTING_BOOLEAN) {
        FC_SettingValue value{};
        if (catalog::equal_ascii_case_insensitive(text, "true") || catalog::equal_ascii_case_insensitive(text, "on") ||
            text == "1") {
            value.boolean_value = FC_TRUE;
            return ResolvedSettingValue::scalar(setting.type, value);
        }
        if (catalog::equal_ascii_case_insensitive(text, "false") ||
            catalog::equal_ascii_case_insensitive(text, "off") || text == "0") {
            value.boolean_value = FC_FALSE;
            return ResolvedSettingValue::scalar(setting.type, value);
        }
        return std::unexpected("expected true/false, 0/1, or off/on");
    }
    // Remaining kinds dispatch to declared-width parsers or to owned string/choice representations.
    switch (setting.type) {
    case FC_SETTING_SIGNED_8:
        return resolve_signed<std::int8_t>(setting, text);
    case FC_SETTING_SIGNED_16:
        return resolve_signed<std::int16_t>(setting, text);
    case FC_SETTING_SIGNED_32:
        return resolve_signed<std::int32_t>(setting, text);
    case FC_SETTING_SIGNED_64:
        return resolve_signed<std::int64_t>(setting, text);
    case FC_SETTING_UNSIGNED_8:
        return resolve_unsigned<std::uint8_t>(setting, text);
    case FC_SETTING_UNSIGNED_16:
        return resolve_unsigned<std::uint16_t>(setting, text);
    case FC_SETTING_UNSIGNED_32:
        return resolve_unsigned<std::uint32_t>(setting, text);
    case FC_SETTING_UNSIGNED_64:
        return resolve_unsigned<std::uint64_t>(setting, text);
    case FC_SETTING_FLOAT_32:
        return resolve_floating<float>(setting, text);
    case FC_SETTING_FLOAT_64:
        return resolve_floating<double>(setting, text);
    case FC_SETTING_STRING: {
        if (!catalog::valid_utf8(text) || (setting.max_length != 0 && text.size() > setting.max_length)) {
            return std::unexpected("expected valid UTF-8 within the declared byte length");
        }
        return ResolvedSettingValue::string(std::string{text});
    }
    case FC_SETTING_CHOICE: {
        const auto found = std::ranges::find_if(setting.choices, [&](const std::string& choice) {
            return catalog::equal_ascii_case_insensitive(choice, text);
        });
        if (found == setting.choices.end()) {
            return std::unexpected("expected one of the declared choice spellings");
        }
        FC_SettingValue value{};
        value.choice_index = static_cast<std::uint32_t>(found - setting.choices.begin());
        return ResolvedSettingValue::scalar(setting.type, value);
    }
    default:
        return std::unexpected("unknown setting type");
    }
}

std::string format_setting_default(const catalog::SettingDefinitionRecord& setting) {
    // Emit the same canonical text grammar consumed by resolution so generated defaults always round-trip.
    switch (setting.type) {
    case FC_SETTING_BOOLEAN:
        return setting.default_value.boolean_value == FC_TRUE ? "true" : "false";
    case FC_SETTING_SIGNED_8:
    case FC_SETTING_SIGNED_16:
    case FC_SETTING_SIGNED_32:
    case FC_SETTING_SIGNED_64:
        return format_number(setting.default_value.signed_value);
    case FC_SETTING_UNSIGNED_8:
    case FC_SETTING_UNSIGNED_16:
    case FC_SETTING_UNSIGNED_32:
    case FC_SETTING_UNSIGNED_64:
        return format_number(setting.default_value.unsigned_value);
    case FC_SETTING_FLOAT_32:
        return format_number(static_cast<float>(setting.default_value.floating_value));
    case FC_SETTING_FLOAT_64:
        return format_number(setting.default_value.floating_value);
    case FC_SETTING_STRING:
        return setting.default_string;
    case FC_SETTING_CHOICE:
        return setting.choices[setting.default_value.choice_index];
    default:
        return {};
    }
}

} // namespace fc::config
