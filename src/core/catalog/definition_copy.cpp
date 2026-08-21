#include "definition_copy.hpp"

#include "../config/configuration_types.hpp"
#include "../targets/target_profiles.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <ranges>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace fc::catalog {
namespace {

constexpr std::size_t kGeneratedLogicalLineByteCapacity = 4096;

// Charges both pointer/count arrays and copied string bytes before traversing plugin-owned metadata.
class MetadataBudget {
  public:
    [[nodiscard]] bool consume_bytes(std::size_t byte_size) noexcept {
        if (byte_size > kPluginMetadataByteCapacity - used_byte_size_) {
            return false;
        }
        used_byte_size_ += byte_size;
        return true;
    }

    [[nodiscard]] bool consume_array(std::uint32_t count, std::size_t element_byte_size) noexcept {
        if (count != 0 && element_byte_size > std::numeric_limits<std::size_t>::max() / count) {
            return false;
        }
        return consume_bytes(static_cast<std::size_t>(count) * element_byte_size);
    }

  private:
    std::size_t used_byte_size_{};
};

// Copy helpers charge each borrowed view before dereferencing it and preserve declaration order in owned records.
[[nodiscard]] std::expected<std::string, std::string> copy_string(FC_StringView view, MetadataBudget& budget) {
    if (!budget.consume_bytes(view.size)) {
        return std::unexpected("The plugin exceeds the copied metadata capacity");
    }
    if (view.size != 0 && view.data == nullptr) {
        return std::unexpected("A nonempty string view has a null pointer");
    }
    if (view.size == 0) {
        return std::string{};
    }
    return std::string{view.data, view.size};
}

template <class Native>
[[nodiscard]] std::expected<void, std::string> validate_array_view(const Native* data, std::uint32_t count,
                                                                   MetadataBudget& budget) {
    if (!budget.consume_array(count, sizeof(Native))) {
        return std::unexpected("The plugin exceeds the copied metadata capacity");
    }
    if (count != 0 && data == nullptr) {
        return std::unexpected("A nonempty definition array has a null pointer");
    }
    return {};
}

[[nodiscard]] std::expected<std::vector<std::string>, std::string>
copy_string_array(const FC_StringView* data, std::uint32_t count, MetadataBudget& budget) {
    auto view = validate_array_view(data, count, budget);
    if (!view) {
        return std::unexpected(std::move(view.error()));
    }
    std::vector<std::string> result;
    result.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        auto value = copy_string(data[index], budget);
        if (!value) {
            return std::unexpected(std::move(value.error()));
        }
        result.push_back(std::move(*value));
    }
    return result;
}

[[nodiscard]] std::expected<SettingDefinitionRecord, std::string> copy_setting(const FC_SettingDefinition& setting,
                                                                               MetadataBudget& budget) {
    // Copy all textual identity first so no returned record retains a borrowed registration pointer.
    SettingDefinitionRecord result;
    auto section = copy_string(setting.section, budget);
    auto key = copy_string(setting.key, budget);
    auto description = copy_string(setting.description, budget);
    auto environment = copy_string(setting.environment, budget);
    if (!section || !key || !description || !environment) {
        return std::unexpected("A setting string could not be copied within the metadata capacity");
    }
    result.section = std::move(*section);
    result.key = std::move(*key);
    result.description = std::move(*description);
    result.environment = std::move(*environment);
    result.type = setting.type;
    result.has_range = setting.has_range;
    result.default_value = setting.default_value;
    result.minimum = setting.minimum;
    result.maximum = setting.maximum;
    result.max_length = setting.max_length;
    result.choices_pointer_present = setting.choices != nullptr;

    // Only string defaults and choice arrays beside the union require an additional deep copy.
    if (setting.type == FC_SETTING_STRING) {
        auto default_string = copy_string(setting.default_value.string_value, budget);
        if (!default_string) {
            return std::unexpected(std::move(default_string.error()));
        }
        result.default_string = std::move(*default_string);
    }
    auto choices = copy_string_array(setting.choices, setting.choice_count, budget);
    if (!choices) {
        return std::unexpected(std::move(choices.error()));
    }
    result.choices = std::move(*choices);
    return result;
}

[[nodiscard]] std::expected<std::vector<SettingDefinitionRecord>, std::string>
copy_settings(const FC_SettingDefinition* data, std::uint32_t count, MetadataBudget& budget) {
    auto view = validate_array_view(data, count, budget);
    if (!view) {
        return std::unexpected(std::move(view.error()));
    }
    std::vector<SettingDefinitionRecord> result;
    result.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        auto setting = copy_setting(data[index], budget);
        if (!setting) {
            return std::unexpected(std::move(setting.error()));
        }
        result.push_back(std::move(*setting));
    }
    return result;
}

[[nodiscard]] std::expected<SupportDefinitionRecord, std::string> copy_support(const FC_SupportDefinition& support,
                                                                               MetadataBudget& budget) {
    // Copy the fixed support facts first; pointer presence is retained separately for later structural validation.
    SupportDefinitionRecord result;
    result.layouts = support.layouts;
    result.roles = support.roles;
    result.image = support.image;
    result.callbacks = support.callbacks;
    result.has_settings = support.has_settings;
    result.settings_pointer_present = support.settings != nullptr;
    result.failure_policy = support.failure_policy;

    // Descendant arrays share the plugin's one metadata budget and publish only when all three copies succeed.
    auto settings = copy_settings(support.settings, support.setting_count, budget);
    auto depends_on = copy_string_array(support.depends_on, support.depends_on_count, budget);
    auto includes = copy_string_array(support.includes, support.include_count, budget);
    if (!settings || !depends_on || !includes) {
        return std::unexpected("A support definition could not be copied within the metadata capacity");
    }
    result.settings = std::move(*settings);
    result.depends_on = std::move(*depends_on);
    result.includes = std::move(*includes);
    return result;
}

[[nodiscard]] std::expected<PatchDefinitionRecord, std::string> copy_patch(const FC_PatchDefinition& patch,
                                                                           MetadataBudget& budget) {
    // Patch metadata is copied before child views so failure never publishes a partially connected tree.
    PatchDefinitionRecord result;
    auto id = copy_string(patch.id, budget);
    auto name = copy_string(patch.name, budget);
    auto description = copy_string(patch.description, budget);
    auto version = copy_string(patch.version, budget);
    auto author = copy_string(patch.author, budget);
    auto source = copy_string(patch.source, budget);
    auto category = copy_string(patch.category, budget);
    if (!id || !name || !description || !version || !author || !source || !category) {
        return std::unexpected("A patch string could not be copied within the metadata capacity");
    }
    result.id = std::move(*id);
    result.name = std::move(*name);
    result.description = std::move(*description);
    result.version = std::move(*version);
    result.author = std::move(*author);
    result.source = std::move(*source);
    result.configurable = patch.configurable;
    result.enabled = patch.enabled;
    result.category = std::move(*category);
    result.failure_policy = patch.failure_policy;

    // Child arrays retain declaration order because later support selection and configuration depend on their ordinals.
    auto settings = copy_settings(patch.settings, patch.setting_count, budget);
    auto depends_on = copy_string_array(patch.depends_on, patch.depends_on_count, budget);
    auto includes = copy_string_array(patch.includes, patch.include_count, budget);
    auto supports_view = validate_array_view(patch.supports, patch.support_count, budget);
    if (!settings || !depends_on || !includes || !supports_view) {
        return std::unexpected("A patch child array could not be copied within the metadata capacity");
    }
    result.settings = std::move(*settings);
    result.depends_on = std::move(*depends_on);
    result.includes = std::move(*includes);
    result.supports.reserve(patch.support_count);
    for (std::uint32_t index = 0; index < patch.support_count; ++index) {
        auto support = copy_support(patch.supports[index], budget);
        if (!support) {
            return std::unexpected(std::move(support.error()));
        }
        result.supports.push_back(std::move(*support));
    }
    return result;
}

[[nodiscard]] std::expected<GroupDefinitionRecord, std::string> copy_group(const FC_GroupDefinition& group,
                                                                           MetadataBudget& budget) {
    GroupDefinitionRecord result;
    auto id = copy_string(group.id, budget);
    auto category = copy_string(group.category, budget);
    auto description = copy_string(group.description, budget);
    auto members = copy_string_array(group.members, group.member_count, budget);
    if (!id || !category || !description || !members) {
        return std::unexpected("A group definition could not be copied within the metadata capacity");
    }
    result.id = std::move(*id);
    result.members = std::move(*members);
    result.configurable = group.configurable;
    result.enabled = group.enabled;
    result.category = std::move(*category);
    result.description = std::move(*description);
    return result;
}

[[nodiscard]] bool canonical_boolean(FC_Bool value) noexcept {
    return value == FC_FALSE || value == FC_TRUE;
}

// Validates complete scalar UTF-8 while rejecting embedded NULs, overlong encodings, and surrogate code points.
[[nodiscard]] bool valid_utf8_bytes(std::string_view value) noexcept {
    std::size_t index = 0;
    while (index < value.size()) {
        const auto first = static_cast<unsigned char>(value[index]);
        if (first == 0) {
            return false;
        }
        if (first <= 0x7f) {
            ++index;
            continue;
        }

        // Decode only enough state to reject invalid leading ranges, truncation, and non-scalar results.
        std::size_t length{};
        std::uint32_t code_point{};
        if (first >= 0xc2 && first <= 0xdf) {
            length = 2;
            code_point = first & 0x1fU;
        } else if (first >= 0xe0 && first <= 0xef) {
            length = 3;
            code_point = first & 0x0fU;
        } else if (first >= 0xf0 && first <= 0xf4) {
            length = 4;
            code_point = first & 0x07U;
        } else {
            return false;
        }
        if (length > value.size() - index) {
            return false;
        }
        // Every continuation contributes exactly six bits after its marker has been validated.
        for (std::size_t continuation = 1; continuation < length; ++continuation) {
            const auto byte = static_cast<unsigned char>(value[index + continuation]);
            if ((byte & 0xc0U) != 0x80U) {
                return false;
            }
            code_point = (code_point << 6U) | (byte & 0x3fU);
        }
        if ((length == 3 && code_point < 0x800U) || (length == 4 && code_point < 0x10000U) ||
            (code_point >= 0xd800U && code_point <= 0xdfffU) || code_point > 0x10ffffU) {
            return false;
        }
        index += length;
    }
    return true;
}

// Structural text helpers define the identities and scalar forms that admission and configuration may compare or emit.
[[nodiscard]] bool valid_id(std::string_view value) noexcept {
    if (value.empty() || value.size() > 64) {
        return false;
    }
    const auto letter = [](char character) {
        return (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z');
    };
    if (!letter(value.front())) {
        return false;
    }
    return std::ranges::all_of(value, [&](char character) {
        return letter(character) || (character >= '0' && character <= '9') || character == '_';
    });
}

// FusionCutter identifies the framework itself, so a case-only spelling change cannot make it a plugin identity.
[[nodiscard]] bool reserved_plugin_id(std::string_view value) noexcept {
    return equal_ascii_case_insensitive(value, "FusionCutter");
}

// Patch, group, and category IDs additionally reserve General because those IDs can become complete INI sections.
[[nodiscard]] bool reserved_catalog_id(std::string_view value) noexcept {
    return reserved_plugin_id(value) || equal_ascii_case_insensitive(value, "General");
}

[[nodiscard]] bool valid_setting_key(std::string_view value) noexcept {
    if (value.empty() || value.front() == ';' || value.front() == '#' || value.front() == ' ' ||
        value.front() == '\t' || value.back() == ' ' || value.back() == '\t') {
        return false;
    }
    return std::ranges::all_of(value, [](char character) {
        const auto byte = static_cast<unsigned char>(character);
        return byte <= 0x7fU && byte != 0 && character != '\r' && character != '\n' && character != '=' &&
               character != ':';
    });
}

[[nodiscard]] bool valid_environment(std::string_view value) noexcept {
    return !value.empty() && std::ranges::all_of(value, [](char character) {
        return (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') ||
               (character >= '0' && character <= '9') || character == '_';
    });
}

[[nodiscard]] bool valid_generated_scalar(std::string_view value) noexcept {
    const bool quoted = value.size() >= 2 && ((value.front() == '\'' && value.back() == '\'') ||
                                              (value.front() == '"' && value.back() == '"'));
    return !quoted && valid_utf8_bytes(value) &&
           (value.empty() ||
            (value.front() != ' ' && value.front() != '\t' && value.back() != ' ' && value.back() != '\t')) &&
           value.find('\r') == std::string_view::npos && value.find('\n') == std::string_view::npos;
}

[[nodiscard]] bool signed_fits(FC_SettingType type, std::int64_t value) noexcept {
    switch (type) {
    case FC_SETTING_SIGNED_8:
        return value >= std::numeric_limits<std::int8_t>::min() && value <= std::numeric_limits<std::int8_t>::max();
    case FC_SETTING_SIGNED_16:
        return value >= std::numeric_limits<std::int16_t>::min() && value <= std::numeric_limits<std::int16_t>::max();
    case FC_SETTING_SIGNED_32:
        return value >= std::numeric_limits<std::int32_t>::min() && value <= std::numeric_limits<std::int32_t>::max();
    case FC_SETTING_SIGNED_64:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] bool unsigned_fits(FC_SettingType type, std::uint64_t value) noexcept {
    switch (type) {
    case FC_SETTING_UNSIGNED_8:
        return value <= std::numeric_limits<std::uint8_t>::max();
    case FC_SETTING_UNSIGNED_16:
        return value <= std::numeric_limits<std::uint16_t>::max();
    case FC_SETTING_UNSIGNED_32:
        return value <= std::numeric_limits<std::uint32_t>::max();
    case FC_SETTING_UNSIGNED_64:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] bool floating_fits(FC_SettingType type, double value) noexcept {
    if (!std::isfinite(value)) {
        return false;
    }
    if (type == FC_SETTING_FLOAT_64) {
        return true;
    }
    if (type == FC_SETTING_FLOAT_32) {
        return std::isfinite(static_cast<float>(value));
    }
    return false;
}

// Validates both the active union member and every constraint implied by one setting's declared type.
[[nodiscard]] std::expected<void, std::string> validate_setting(const SettingDefinitionRecord& setting) {
    if ((!setting.section.empty() && !valid_id(setting.section)) || !valid_setting_key(setting.key) ||
        !valid_utf8_bytes(setting.description) ||
        (!setting.environment.empty() && !valid_environment(setting.environment)) ||
        !canonical_boolean(setting.has_range)) {
        return std::unexpected("A setting has invalid identity, text, environment, or presence metadata");
    }
    if (setting.type != FC_SETTING_CHOICE && (setting.choices_pointer_present || !setting.choices.empty())) {
        return std::unexpected("Only a Choice setting may supply choices");
    }
    if (setting.type != FC_SETTING_STRING && setting.max_length != 0) {
        return std::unexpected("Only a String setting may supply max_length");
    }

    // Width-specific checks happen before range checks so a transport value cannot be accepted through narrowing.
    bool valid_default{};
    bool valid_range = setting.has_range == FC_FALSE;
    switch (setting.type) {
    case FC_SETTING_BOOLEAN:
        valid_default = canonical_boolean(setting.default_value.boolean_value);
        break;
    case FC_SETTING_SIGNED_8:
    case FC_SETTING_SIGNED_16:
    case FC_SETTING_SIGNED_32:
    case FC_SETTING_SIGNED_64:
        valid_default = signed_fits(setting.type, setting.default_value.signed_value);
        if (setting.has_range == FC_TRUE) {
            valid_range = signed_fits(setting.type, setting.minimum.signed_value) &&
                          signed_fits(setting.type, setting.maximum.signed_value) &&
                          setting.minimum.signed_value <= setting.maximum.signed_value &&
                          setting.default_value.signed_value >= setting.minimum.signed_value &&
                          setting.default_value.signed_value <= setting.maximum.signed_value;
        }
        break;
    case FC_SETTING_UNSIGNED_8:
    case FC_SETTING_UNSIGNED_16:
    case FC_SETTING_UNSIGNED_32:
    case FC_SETTING_UNSIGNED_64:
        valid_default = unsigned_fits(setting.type, setting.default_value.unsigned_value);
        if (setting.has_range == FC_TRUE) {
            valid_range = unsigned_fits(setting.type, setting.minimum.unsigned_value) &&
                          unsigned_fits(setting.type, setting.maximum.unsigned_value) &&
                          setting.minimum.unsigned_value <= setting.maximum.unsigned_value &&
                          setting.default_value.unsigned_value >= setting.minimum.unsigned_value &&
                          setting.default_value.unsigned_value <= setting.maximum.unsigned_value;
        }
        break;
    case FC_SETTING_FLOAT_32:
    case FC_SETTING_FLOAT_64:
        valid_default = floating_fits(setting.type, setting.default_value.floating_value);
        if (setting.has_range == FC_TRUE) {
            const auto value = setting.type == FC_SETTING_FLOAT_32
                                   ? static_cast<double>(static_cast<float>(setting.default_value.floating_value))
                                   : setting.default_value.floating_value;
            const auto minimum = setting.type == FC_SETTING_FLOAT_32
                                     ? static_cast<double>(static_cast<float>(setting.minimum.floating_value))
                                     : setting.minimum.floating_value;
            const auto maximum = setting.type == FC_SETTING_FLOAT_32
                                     ? static_cast<double>(static_cast<float>(setting.maximum.floating_value))
                                     : setting.maximum.floating_value;
            valid_range = floating_fits(setting.type, setting.minimum.floating_value) &&
                          floating_fits(setting.type, setting.maximum.floating_value) && minimum <= maximum &&
                          value >= minimum && value <= maximum;
        }
        break;
    case FC_SETTING_STRING:
        valid_default = valid_generated_scalar(setting.default_string) &&
                        (setting.max_length == 0 || setting.default_string.size() <= setting.max_length);
        break;
    case FC_SETTING_CHOICE: {
        valid_default = setting.choices_pointer_present && !setting.choices.empty() &&
                        setting.default_value.choice_index < setting.choices.size();
        std::unordered_set<std::string> names;
        for (const auto& choice : setting.choices) {
            valid_default = valid_default && valid_generated_scalar(choice) && names.insert(fold_ascii(choice)).second;
        }
        break;
    }
    default:
        return std::unexpected("A setting has an unknown type");
    }
    if (setting.has_range == FC_TRUE && !(setting.type >= FC_SETTING_SIGNED_8 && setting.type <= FC_SETTING_FLOAT_64)) {
        valid_range = false;
    }
    if (!valid_default || !valid_range) {
        return std::unexpected("A setting default or constraint is invalid for its declared type");
    }

    // Structural line validation calls the configuration formatter so generation has no parallel scalar spelling.
    const auto value = config::format_setting_default(setting);
    if (setting.key.size() + 1U + value.size() > kGeneratedLogicalLineByteCapacity) {
        return std::unexpected("A generated setting default exceeds the capacity for one logical line");
    }
    return {};
}

[[nodiscard]] std::expected<void, std::string> validate_settings(const std::vector<SettingDefinitionRecord>& settings) {
    std::unordered_set<std::string> identities;
    for (const auto& setting : settings) {
        auto valid = validate_setting(setting);
        if (!valid) {
            return valid;
        }
        const auto identity = fold_ascii(setting.section) + "\n" + fold_ascii(setting.key);
        if (!identities.insert(identity).second) {
            return std::unexpected("A settings schema repeats a key in one section");
        }
    }
    return {};
}

[[nodiscard]] bool valid_relationships(const std::vector<std::string>& relationships) noexcept {
    return std::ranges::all_of(relationships, valid_id);
}

[[nodiscard]] bool valid_callback(std::uintptr_t callback, const CodeOwner& owner) noexcept {
    return callback == 0 || owner.contains_executable(callback);
}

[[nodiscard]] std::expected<void, std::string> validate_callbacks(const FC_PatchCallbacks& callbacks,
                                                                  const CodeOwner& owner) {
    // Create, Destroy, and Plan define the minimum lifecycle needed for every admitted support record.
    if (callbacks.create == nullptr || callbacks.destroy == nullptr || callbacks.plan == nullptr) {
        return std::unexpected("Every support requires Create, Destroy, and Plan callbacks");
    }
    const std::uintptr_t callback_addresses[]{
        reinterpret_cast<std::uintptr_t>(callbacks.create),
        reinterpret_cast<std::uintptr_t>(callbacks.destroy),
        reinterpret_cast<std::uintptr_t>(callbacks.plan),
        reinterpret_cast<std::uintptr_t>(callbacks.prepare),
        reinterpret_cast<std::uintptr_t>(callbacks.activate),
        reinterpret_cast<std::uintptr_t>(callbacks.update),
        reinterpret_cast<std::uintptr_t>(callbacks.write_status),
        reinterpret_cast<std::uintptr_t>(callbacks.query_interface),
    };
    // Optional callbacks are accepted only when their non-null addresses belong to the contribution's code owner.
    if (!std::ranges::all_of(callback_addresses, [&](std::uintptr_t address) {
            return valid_callback(address, owner);
        })) {
        return std::unexpected("A patch callback does not belong to the contribution's executable code owner");
    }
    return {};
}

// Expands a support's masks against the complete target matrix and validates every retained callback owner.
[[nodiscard]] std::expected<void, std::string> validate_support(const SupportDefinitionRecord& support,
                                                                const CodeOwner& owner) {
    if (support.layouts == 0 || (support.layouts & ~FC_LAYOUT_ALL) != 0 ||
        (support.roles != FC_HOST_ROLE_CLIENT && support.roles != FC_HOST_ROLE_SERVER &&
         support.roles != FC_HOST_ROLE_ALL) ||
        (support.image != FC_IMAGE_GAME && support.image != FC_IMAGE_BOOTSTRAP &&
         support.image != FC_IMAGE_GALAXY_PEER) ||
        !canonical_boolean(support.has_settings) ||
        (support.failure_policy != FC_FAILURE_INHERIT && support.failure_policy != FC_FAILURE_CONTINUE &&
         support.failure_policy != FC_FAILURE_FATAL)) {
        return std::unexpected("A support has an invalid mask, image, presence value, or failure policy");
    }
    if (support.has_settings == FC_FALSE && (support.settings_pointer_present || !support.settings.empty())) {
        return std::unexpected("A support that inherits settings must use a null, empty settings view");
    }
    // Expand the masks explicitly so every selected layout/role/image tuple belongs to the supported target matrix.
    constexpr FC_TargetLayout layouts[]{FC_LAYOUT_GAMESPY_RETAIL, FC_LAYOUT_STEAM_RETAIL, FC_LAYOUT_GOG_RETAIL,
                                        FC_LAYOUT_MOD_TOOLS, FC_LAYOUT_CLASSIC_COLLECTION};
    constexpr FC_HostRole roles[]{FC_HOST_ROLE_CLIENT, FC_HOST_ROLE_SERVER};
    for (const auto layout : layouts) {
        if ((support.layouts & layout) == 0) {
            continue;
        }
        for (const auto role : roles) {
            if ((support.roles & role) != 0 && !targets::valid_target_tuple(layout, role, support.image)) {
                return std::unexpected("A support expands to a tuple outside the target matrix for ABI generation 1");
            }
        }
    }
    // Retained code, schema, and relationship checks apply only after the target tuple has been admitted.
    auto callbacks = validate_callbacks(support.callbacks, owner);
    auto settings = validate_settings(support.settings);
    if (!callbacks) {
        return callbacks;
    }
    if (!settings) {
        return settings;
    }
    if (!valid_relationships(support.depends_on) || !valid_relationships(support.includes)) {
        return std::unexpected("A support contains an invalid relationship ID");
    }
    return {};
}

// Validates one patch as a self-contained unit before plugin-wide IDs and references are considered.
[[nodiscard]] std::expected<void, std::string> validate_patch(const PatchDefinitionRecord& patch,
                                                              const CodeOwner& owner) {
    if (!valid_id(patch.id) || reserved_catalog_id(patch.id) || patch.name.empty() || !valid_utf8_bytes(patch.name) ||
        !valid_utf8_bytes(patch.description) || !valid_utf8_bytes(patch.version) || !valid_utf8_bytes(patch.author) ||
        !valid_utf8_bytes(patch.source) || (!patch.category.empty() && !valid_id(patch.category)) ||
        !canonical_boolean(patch.configurable) || !canonical_boolean(patch.enabled) ||
        (patch.failure_policy != FC_FAILURE_CONTINUE && patch.failure_policy != FC_FAILURE_FATAL) ||
        patch.supports.empty() || !valid_relationships(patch.depends_on) || !valid_relationships(patch.includes)) {
        return std::unexpected("A patch has invalid required metadata");
    }
    auto settings = validate_settings(patch.settings);
    if (!settings) {
        return settings;
    }
    for (std::size_t index = 0; index < patch.supports.size(); ++index) {
        auto support = validate_support(patch.supports[index], owner);
        if (!support) {
            return support;
        }
        // GalaxyPeer is the late image in ABI generation 1; its outcome is unknown until after startup has completed.
        const auto effective_policy = patch.supports[index].failure_policy == FC_FAILURE_INHERIT
                                          ? patch.failure_policy
                                          : patch.supports[index].failure_policy;
        if (patch.supports[index].image == FC_IMAGE_GALAXY_PEER && effective_policy == FC_FAILURE_FATAL) {
            return std::unexpected("A support for a late image cannot use the fatal failure policy");
        }
        for (std::size_t prior = 0; prior < index; ++prior) {
            if ((patch.supports[prior].layouts & patch.supports[index].layouts) != 0 &&
                (patch.supports[prior].roles & patch.supports[index].roles) != 0) {
                return std::unexpected("A patch has overlapping support definitions");
            }
        }
    }
    return {};
}

} // namespace

std::expected<PluginDefinitionRecord, std::string> copy_plugin_definition(const FC_PluginDefinition* plugin) {
    if (plugin == nullptr) {
        return std::unexpected("The registry received a null plugin definition");
    }
    constexpr auto required_size = offsetof(FC_PluginDefinition, patch_count) + sizeof(std::uint32_t);
    if (plugin->struct_size < required_size) {
        return std::unexpected("The plugin definition does not contain the prefix required by ABI generation 1");
    }

    // Charge the root and all three child arrays before entering plugin-controlled memory.
    MetadataBudget budget;
    if (!budget.consume_bytes(sizeof(FC_PluginDefinition))) {
        return std::unexpected("The plugin exceeds the copied metadata capacity");
    }
    auto categories_view = validate_array_view(plugin->categories, plugin->category_count, budget);
    auto groups_view = validate_array_view(plugin->groups, plugin->group_count, budget);
    auto patches_view = validate_array_view(plugin->patches, plugin->patch_count, budget);
    if (!categories_view || !groups_view || !patches_view) {
        return std::unexpected("A top-level definition array is malformed or exceeds the metadata capacity");
    }

    // Copy root strings first, then descend through the fixed category/group/patch tree in declaration order.
    PluginDefinitionRecord result;
    auto id = copy_string(plugin->id, budget);
    auto version = copy_string(plugin->version, budget);
    auto author = copy_string(plugin->author, budget);
    auto source = copy_string(plugin->source, budget);
    if (!id || !version || !author || !source) {
        return std::unexpected("A plugin string could not be copied within the metadata capacity");
    }
    result.id = std::move(*id);
    result.version = std::move(*version);
    result.author = std::move(*author);
    result.source = std::move(*source);

    result.categories.reserve(plugin->category_count);
    for (std::uint32_t index = 0; index < plugin->category_count; ++index) {
        auto category_id = copy_string(plugin->categories[index].id, budget);
        if (!category_id) {
            return std::unexpected(std::move(category_id.error()));
        }
        result.categories.push_back(
            {std::move(*category_id), plugin->categories[index].has_order, plugin->categories[index].order});
    }
    result.groups.reserve(plugin->group_count);
    for (std::uint32_t index = 0; index < plugin->group_count; ++index) {
        auto group = copy_group(plugin->groups[index], budget);
        if (!group) {
            return std::unexpected(std::move(group.error()));
        }
        result.groups.push_back(std::move(*group));
    }
    result.patches.reserve(plugin->patch_count);
    for (std::uint32_t index = 0; index < plugin->patch_count; ++index) {
        auto patch = copy_patch(plugin->patches[index], budget);
        if (!patch) {
            return std::unexpected(std::move(patch.error()));
        }
        result.patches.push_back(std::move(*patch));
    }
    return result;
}

std::expected<void, std::string> validate_plugin_definition(const PluginDefinitionRecord& plugin, PluginOrigin origin,
                                                            const CodeOwner& code_owner) {
    if (!valid_id(plugin.id) || !valid_utf8_bytes(plugin.version) || !valid_utf8_bytes(plugin.author) ||
        !valid_utf8_bytes(plugin.source)) {
        return std::unexpected("A plugin has invalid identity or descriptive metadata");
    }
    if (reserved_plugin_id(plugin.id)) {
        return std::unexpected("FusionCutter is a reserved plugin ID");
    }
    if (origin == PluginOrigin::Core && plugin.id != "Core") {
        return std::unexpected("The built-in Core plugin must use the canonical plugin ID Core");
    }
    if (origin != PluginOrigin::Core && plugin.patches.empty()) {
        return std::unexpected("Only the built-in Core plugin may contain no patches");
    }

    // Build complete case-folded identity sets while validating each owned definition exactly once.
    std::unordered_set<std::string> catalog_ids;
    std::unordered_set<std::string> category_ids;
    std::unordered_set<std::string> patch_ids;
    const auto add_id = [&](std::string_view id) -> bool {
        return valid_id(id) && !reserved_catalog_id(id) && catalog_ids.insert(fold_ascii(id)).second;
    };
    for (const auto& category : plugin.categories) {
        if (!add_id(category.id) || !canonical_boolean(category.has_order) ||
            (category.has_order == FC_FALSE && category.order != 0)) {
            return std::unexpected("A category has invalid or duplicate metadata");
        }
        category_ids.insert(fold_ascii(category.id));
    }
    for (const auto& group : plugin.groups) {
        if (!add_id(group.id) || !canonical_boolean(group.configurable) || !canonical_boolean(group.enabled) ||
            (group.configurable == FC_FALSE && group.enabled != FC_FALSE) ||
            (!group.category.empty() && !valid_id(group.category)) || !valid_utf8_bytes(group.description)) {
            return std::unexpected("A group has invalid required metadata");
        }
    }
    for (const auto& patch : plugin.patches) {
        if (!add_id(patch.id)) {
            return std::unexpected("Patch, group, and category IDs must be distinct within one plugin");
        }
        patch_ids.insert(fold_ascii(patch.id));
        auto valid = validate_patch(patch, code_owner);
        if (!valid) {
            return valid;
        }
    }

    // Cross-references are checked only after all category and patch identities are known.
    std::unordered_set<std::string> grouped_patches;
    for (const auto& group : plugin.groups) {
        if (!group.category.empty() && !category_ids.contains(fold_ascii(group.category))) {
            return std::unexpected("A group references a category not declared by its plugin");
        }
        std::unordered_set<std::string> local_members;
        for (const auto& member : group.members) {
            const auto folded = fold_ascii(member);
            if (!valid_id(member) || !patch_ids.contains(folded) || !local_members.insert(folded).second ||
                !grouped_patches.insert(folded).second) {
                return std::unexpected("A group member is missing, repeated, non-owned, or multiply grouped");
            }
        }
    }
    for (const auto& patch : plugin.patches) {
        if (!patch.category.empty() && !category_ids.contains(fold_ascii(patch.category))) {
            return std::unexpected("A patch references a category not declared by its plugin");
        }
    }
    return {};
}

bool equal_ascii_case_insensitive(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        auto left_character = left[index];
        auto right_character = right[index];
        if (left_character >= 'A' && left_character <= 'Z') {
            left_character = static_cast<char>(left_character + ('a' - 'A'));
        }
        if (right_character >= 'A' && right_character <= 'Z') {
            right_character = static_cast<char>(right_character + ('a' - 'A'));
        }
        if (left_character != right_character) {
            return false;
        }
    }
    return true;
}

std::string fold_ascii(std::string_view value) {
    std::string result{value};
    for (auto& character : result) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character + ('a' - 'A'));
        }
    }
    return result;
}

bool valid_utf8(std::string_view value) noexcept {
    return valid_utf8_bytes(value);
}

bool valid_framework_id(std::string_view value) noexcept {
    return valid_id(value);
}

} // namespace fc::catalog
