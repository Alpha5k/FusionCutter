#include "plugin.hpp"

#include <fc/base.hpp>

#include <Windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <cmath>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace fc::runtime {

namespace {

// byte_size must cover every field through patch_count before the checks read the FC_Plugin record.
constexpr std::size_t kRequiredPluginPrefix = offsetof(FC_Plugin, patch_count) + sizeof(FC_Plugin::patch_count);

// Recognizes the ASCII whitespace that generated INI names must avoid at their edges.
bool is_ascii_whitespace(char value) {
    return value == ' ' || value == '\t' || value == '\r' || value == '\n' || value == '\f' || value == '\v';
}

// Reads valid counted UTF-8 into a non-owning view, or fills diagnostic on failure; input storage must outlive the view.
// allow_null permits embedded null bytes in scalar text defaults.
bool read_text_view(FC_Text text, std::string_view field, std::string_view owner, std::string_view &out, Diagnostic &diagnostic,
                    bool allow_null = false) {
    if (text.size != 0 && text.data == nullptr) {
        diagnostic = {std::string{owner},
                      std::string{field} + " declares " + std::to_string(text.size) + " bytes but its data pointer is null"};
        return false;
    }

    out = text.size == 0 ? std::string_view{} : std::string_view{text.data, text.size};
    if (out.size() > INT_MAX || (!allow_null && out.find('\0') != std::string_view::npos) ||
        (!out.empty() && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, out.data(), static_cast<int>(out.size()), nullptr, 0) == 0)) {
        diagnostic = {std::string{owner},
                      std::string{field} + " is too long, contains a prohibited null byte or could not be decoded as UTF-8"};
        return false;
    }
    return true;
}

// A null array pointer is valid only when count is zero.
template <class T>
bool has_valid_array_view(const T *data, std::uint32_t count, std::string_view field, std::string_view owner, Diagnostic &diagnostic) {
    if (count != 0 && data == nullptr) {
        diagnostic = {std::string{owner},
                      "The " + std::string{field} + " array declares " + std::to_string(count) + " entries but its data pointer is null"};
        return false;
    }
    return true;
}

// Section names and keys must survive generated INI text without trimming or changing its structure.
std::optional<std::string> ini_name_failure(std::string_view value, bool key) {
    if (value.empty()) {
        return "must not be empty";
    }
    if (is_ascii_whitespace(value.front()) || is_ascii_whitespace(value.back())) {
        return "must not begin or end with whitespace";
    }
    if (key && (value.front() == ';' || value.front() == '#')) {
        return "must not begin with ';' or '#'";
    }

    for (const auto character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte < 0x20 || byte == 0x7f) {
            return "contains prohibited control byte " + std::to_string(byte);
        }
        if (character == '[' || character == ']' || (key && character == '=')) {
            return "contains prohibited character '" + std::string(1, character) + "'";
        }
    }
    return std::nullopt;
}

// Plugin IDs also become configuration filenames and the first part of qualified dependencies.
std::optional<std::string> plugin_id_failure(std::string_view value) {
    if (auto failure = ini_name_failure(value, false)) {
        return failure;
    }
    if (value.contains('.')) {
        return "must not contain '.'";
    }

    for (const auto character : value) {
        if (std::string_view{"<>:\"/\\|?*"}.contains(character)) {
            return "contains prohibited Windows filename character '" + std::string(1, character) + "'";
        }
    }
    return std::nullopt;
}

// Patch and group IDs become INI keys, where a leading ';' or '#' would hide the declaration.
std::optional<std::string> local_id_failure(std::string_view value) {
    if (auto failure = ini_name_failure(value, true)) {
        return failure;
    }
    if (value.contains('.')) {
        return "must not contain '.'";
    }
    return std::nullopt;
}

// A dependency names a patch or group in this plugin, or uses exactly plugin.local_id.
// In both forms, the patch or group ID must follow configuration key rules.
bool is_valid_dependency_reference(std::string_view value) {
    const auto dot = value.find('.');
    if (dot == std::string_view::npos) {
        return !local_id_failure(value).has_value();
    }
    if (dot == 0 || dot + 1 == value.size() || value.find('.', dot + 1) != std::string_view::npos) {
        return false;
    }
    return !plugin_id_failure(value.substr(0, dot)).has_value() && !local_id_failure(value.substr(dot + 1)).has_value();
}

// Accepts a nonempty exact profile ID or one trailing wildcard, including the bare '*' selector.
bool is_valid_profile_selector(std::string_view selector) {
    if (selector.empty()) {
        return false;
    }
    const auto first = selector.find('*');
    return first == std::string_view::npos || (first == selector.size() - 1 && selector.find('*', first + 1) == std::string_view::npos);
}

// Validates one FC_Plugin record and returns its first error. Pointer/count pairs are checked before dereference.
// ID comparisons are ASCII case-insensitive.
std::optional<Diagnostic> validate_plugin_definition(const DefinitionCandidate &candidate) {
    const auto owner = std::string_view{candidate.owner};
    if (candidate.definition == nullptr) {
        return Diagnostic{candidate.owner, "plugin definition pointer is null"};
    }

    const auto &plugin = *candidate.definition;
    if (plugin.byte_size < kRequiredPluginPrefix) {
        return Diagnostic{candidate.owner, "FC_Plugin record supplies " + std::to_string(plugin.byte_size) + " bytes; at least " +
                                               std::to_string(kRequiredPluginPrefix) + " bytes are required"};
    }

    // Validate plugin text and pointer/count pairs before traversing arrays supplied by the plugin.
    Diagnostic error;
    std::string_view plugin_id;
    std::string_view version;
    if (!read_text_view(plugin.id, "plugin ID", owner, plugin_id, error) ||
        !read_text_view(plugin.version, "plugin version", owner, version, error)) {
        return error;
    }
    if (const auto failure = plugin_id_failure(plugin_id)) {
        return Diagnostic{candidate.owner, "plugin ID '" + std::string{plugin_id} + "' " + *failure};
    }
    if (version.empty()) {
        return Diagnostic{candidate.owner, "plugin version is empty"};
    }

    if (!has_valid_array_view(plugin.categories, plugin.category_count, "categories", owner, error) ||
        !has_valid_array_view(plugin.groups, plugin.group_count, "groups", owner, error) ||
        !has_valid_array_view(plugin.patches, plugin.patch_count, "patches", owner, error)) {
        return error;
    }

    // Categories are collected first because patches and groups refer to them by ID.
    std::unordered_set<std::string> category_ids;
    for (std::uint32_t index = 0; index < plugin.category_count; ++index) {
        const auto &category = plugin.categories[index];
        std::string_view id;
        if (!read_text_view(category.id, "category ID", owner, id, error)) {
            return error;
        }
        if (const auto failure = ini_name_failure(id, false)) {
            return Diagnostic{candidate.owner, "category ID '" + std::string{id} + "' " + *failure};
        }
        if (category.has_order > 1) {
            return Diagnostic{candidate.owner, "category '" + std::string{id} + "' has_order value is " +
                                                   std::to_string(category.has_order) + "; expected 0 or 1"};
        }
        if (!category_ids.insert(fc::detail::folded_id(id)).second) {
            return Diagnostic{candidate.owner, "duplicate category ID '" + std::string{id} + "'"};
        }
    }

    // Group members resolve against patch IDs, while local IDs also detect patch and group conflicts.
    std::unordered_set<std::string> patch_ids;
    std::unordered_set<std::string> local_ids;
    for (std::uint32_t index = 0; index < plugin.patch_count; ++index) {
        const auto &patch = plugin.patches[index];
        std::string_view id;
        std::string_view description;
        std::string_view category;
        if (!read_text_view(patch.id, "patch ID", owner, id, error) ||
            !read_text_view(patch.description, "patch description", owner, description, error) ||
            !read_text_view(patch.category, "patch category", owner, category, error)) {
            return error;
        }
        if (const auto failure = local_id_failure(id)) {
            return Diagnostic{candidate.owner, "patch ID '" + std::string{id} + "' " + *failure};
        }

        const auto folded_id = fc::detail::folded_id(id);
        if (!local_ids.insert(folded_id).second) {
            return Diagnostic{candidate.owner, "patch or group ID '" + std::string{id} + "' duplicates another patch or group ID"};
        }
        patch_ids.insert(folded_id);

        // Dot-free local IDs are already unique; only category names can overlap generated patch sections.
        if (category_ids.contains(folded_id)) {
            return Diagnostic{candidate.owner, "Generated INI section [" + std::string{id} + "] conflicts with a category"};
        }
        if (!category.empty() && !category_ids.contains(fc::detail::folded_id(category))) {
            return Diagnostic{candidate.owner,
                              "patch '" + std::string{id} + "' references unknown category '" + std::string{category} + "'"};
        }

        // Boolean and enum fields cross a C boundary, so reject values outside their declared domains.
        if (patch.enabled > 1) {
            return Diagnostic{candidate.owner,
                              "patch '" + std::string{id} + "' has enabled value " + std::to_string(patch.enabled) + "; expected 0 or 1"};
        }
        if (patch.configurable > 1) {
            return Diagnostic{candidate.owner, "patch '" + std::string{id} + "' has configurable value " +
                                                   std::to_string(patch.configurable) + "; expected 0 or 1"};
        }
        if (patch.failure_policy != FC_FAILURE_CONTINUE && patch.failure_policy != FC_FAILURE_FATAL) {
            return Diagnostic{candidate.owner, "patch '" + std::string{id} + "' has failure policy " +
                                                   std::to_string(patch.failure_policy) + "; expected 0 (Continue) or 1 (Fatal)"};
        }
        if (patch.update_interval_ms <= 0) {
            return Diagnostic{candidate.owner, "patch '" + std::string{id} + "' update interval must be greater than 0 milliseconds"};
        }

        if (!has_valid_array_view(patch.depends_on, patch.dependency_count, "patch dependencies", owner, error) ||
            !has_valid_array_view(patch.variants, patch.variant_count, "patch variants", owner, error)) {
            return error;
        }

        // Validate dependency syntax now; resolution against admitted plugins happens after admission.
        for (std::uint32_t dependency_index = 0; dependency_index < patch.dependency_count; ++dependency_index) {
            std::string_view dependency;
            if (!read_text_view(patch.depends_on[dependency_index], "patch dependency", owner, dependency, error)) {
                return error;
            }
            if (!is_valid_dependency_reference(dependency)) {
                return Diagnostic{candidate.owner, "patch '" + std::string{id} + "' dependency '" + std::string{dependency} +
                                                       "' must name a patch or group in this plugin, or use exactly plugin.local_id"};
            }
        }

        // Each variant supplies a valid role and policy, a handler factory, and at least one required image record.
        for (std::uint32_t variant_index = 0; variant_index < patch.variant_count; ++variant_index) {
            const auto &variant = patch.variants[variant_index];
            if (variant.role != FC_ROLE_CLIENT && variant.role != FC_ROLE_SERVER && variant.role != FC_ROLE_ALL) {
                return Diagnostic{candidate.owner, "patch '" + std::string{id} + "' variant has role " + std::to_string(variant.role) +
                                                       "; expected 1 (Client), 2 (Server), or 3 (All)"};
            }
            if (variant.failure_policy != FC_FAILURE_CONTINUE && variant.failure_policy != FC_FAILURE_FATAL) {
                return Diagnostic{candidate.owner, "patch '" + std::string{id} + "' variant has failure policy " +
                                                       std::to_string(variant.failure_policy) + "; expected 0 (Continue) or 1 (Fatal)"};
            }
            if (variant.factory.create == nullptr) {
                return Diagnostic{candidate.owner, "patch '" + std::string{id} + "' variant must provide a handler factory"};
            }
            if (!has_valid_array_view(variant.settings, variant.setting_count, "variant settings", owner, error) ||
                !has_valid_array_view(variant.images, variant.image_count, "variant images", owner, error) ||
                !has_valid_array_view(variant.depends_on, variant.dependency_count, "variant dependencies", owner, error)) {
                return error;
            }
            if (variant.image_count == 0) {
                return Diagnostic{candidate.owner, "patch '" + std::string{id} + "' variant must declare at least one required image"};
            }

            // Defaults must pass the same plugin conversion used for selected values and fallback.
            std::unordered_set<std::string> settings;
            for (std::uint32_t setting_index = 0; setting_index != variant.setting_count; ++setting_index) {
                const auto &setting = variant.settings[setting_index];
                std::string_view key, section, help, environment, constraints;
                if (!read_text_view(setting.key, "setting key", owner, key, error) ||
                    !read_text_view(setting.section, "setting subsection", owner, section, error) ||
                    !read_text_view(setting.description, "setting description", owner, help, error) ||
                    !read_text_view(setting.environment, "setting environment", owner, environment, error) ||
                    !read_text_view(setting.constraints, "setting constraints", owner, constraints, error)) {
                    return error;
                }
                if (ini_name_failure(key, true) || (!section.empty() && ini_name_failure(section, false))) {
                    return Diagnostic{candidate.owner, "Patch '" + std::string{id} + "' has an invalid setting key or subsection"};
                }
                // The first dot separates the unique patch ID from its subsection name.
                if (!section.empty() && category_ids.contains(fc::detail::folded_id(std::string{id} + "." + std::string{section}))) {
                    return Diagnostic{candidate.owner, "Generated INI subsection for a patch conflicts with another declaration"};
                }
                if (!settings.insert(fc::detail::folded_id(section) + "\n" + fc::detail::folded_id(key)).second) {
                    return Diagnostic{candidate.owner, "Patch '" + std::string{id} + "' has a duplicate setting key"};
                }
                if (patch.configurable && category.empty() && section.empty() && fc::detail::folded_id(key) == "enabled") {
                    return Diagnostic{candidate.owner, "Patch '" + std::string{id} + "' setting conflicts with its enabled control"};
                }

                if (setting.default_value.kind > FC_SCALAR_TEXT || !setting.validate) {
                    return Diagnostic{candidate.owner,
                                      "Setting '" + std::string{key} + "' has an invalid default value type or no validation callback"};
                }
                if (setting.default_value.kind == FC_SCALAR_TEXT &&
                    !read_text_view(setting.default_value.value.text, "setting default", owner, help, error, true)) {
                    return error;
                }

                Diagnostic rejected{candidate.owner, "No further reason was reported"};
                // Copy the validator's callback-scoped error into the diagnostic returned to the caller.
                const auto capture = [](void *context, FC_Text message, std::uint32_t code) {
                    auto &diagnostic = *static_cast<Diagnostic *>(context);
                    diagnostic.message =
                        message.size && message.data ? std::string{message.data, message.size} : "No further reason was reported";
                    diagnostic.windows_error = code;
                };

                if ((setting.default_value.kind == FC_SCALAR_BOOL && setting.default_value.value.boolean > 1) ||
                    (setting.default_value.kind == FC_SCALAR_REAL && !std::isfinite(setting.default_value.value.real)) ||
                    setting.validate(setting.context, &setting.default_value, {&rejected, capture}) != 1) {
                    const auto setting_section = std::string{id} + (section.empty() ? "" : "." + std::string{section});
                    rejected.message = "Patch '" + std::string{id} + "' setting [" + setting_section + "]." + std::string{key} +
                                       " default failed validation: " + rejected.message;
                    return rejected;
                }
            }

            // Image labels must be unique within the variant; selectors are checked against their syntax here.
            std::unordered_set<std::string> image_names;
            for (std::uint32_t image_index = 0; image_index < variant.image_count; ++image_index) {
                const auto &image = variant.images[image_index];
                std::string_view name;
                if (!read_text_view(image.name, "image label", owner, name, error)) {
                    return error;
                }
                if (name.empty()) {
                    return Diagnostic{candidate.owner, "patch '" + std::string{id} + "' has an empty image label"};
                }
                if (!image_names.insert(fc::detail::folded_id(name)).second) {
                    return Diagnostic{candidate.owner, "patch '" + std::string{id} + "' has a duplicate image label"};
                }

                if (!has_valid_array_view(image.selectors, image.selector_count, "image selectors", owner, error)) {
                    return error;
                }
                if (image.selector_count == 0) {
                    return Diagnostic{candidate.owner, "image '" + std::string{name} + "' must declare at least one profile selector"};
                }

                for (std::uint32_t selector_index = 0; selector_index < image.selector_count; ++selector_index) {
                    std::string_view selector;
                    if (!read_text_view(image.selectors[selector_index], "image selector", owner, selector, error)) {
                        return error;
                    }
                    if (!is_valid_profile_selector(selector)) {
                        return Diagnostic{candidate.owner, "image '" + std::string{name} + "' selector '" + std::string{selector} +
                                                               "' must be an exact profile ID or end with one '*' wildcard"};
                    }
                }
            }

            for (std::uint32_t dependency_index = 0; dependency_index < variant.dependency_count; ++dependency_index) {
                std::string_view dependency;
                if (!read_text_view(variant.depends_on[dependency_index], "variant dependency", owner, dependency, error)) {
                    return error;
                }
                if (!is_valid_dependency_reference(dependency)) {
                    return Diagnostic{candidate.owner, "patch '" + std::string{id} + "' variant dependency '" + std::string{dependency} +
                                                           "' must name a patch or group in this plugin, or use exactly plugin.local_id"};
                }
            }
        }
    }

    // Remember each patch's first group so we can reject repeated membership.
    std::unordered_map<std::string, std::string> memberships;

    // Check groups after collecting all patch IDs so we can check that their members exist.
    for (std::uint32_t index = 0; index < plugin.group_count; ++index) {
        const auto &group = plugin.groups[index];
        std::string_view id;
        std::string_view description;
        std::string_view category;
        if (!read_text_view(group.id, "group ID", owner, id, error) ||
            !read_text_view(group.description, "group description", owner, description, error) ||
            !read_text_view(group.category, "group category", owner, category, error)) {
            return error;
        }
        if (const auto failure = local_id_failure(id)) {
            return Diagnostic{candidate.owner, "group ID '" + std::string{id} + "' " + *failure};
        }
        if (!local_ids.insert(fc::detail::folded_id(id)).second) {
            return Diagnostic{candidate.owner, "patch or group ID '" + std::string{id} + "' duplicates another patch or group ID"};
        }
        if (!category.empty() && !category_ids.contains(fc::detail::folded_id(category))) {
            return Diagnostic{candidate.owner,
                              "group '" + std::string{id} + "' references unknown category '" + std::string{category} + "'"};
        }

        if (group.enabled > 1) {
            return Diagnostic{candidate.owner,
                              "group '" + std::string{id} + "' has enabled value " + std::to_string(group.enabled) + "; expected 0 or 1"};
        }
        if (group.configurable > 1) {
            return Diagnostic{candidate.owner, "group '" + std::string{id} + "' has configurable value " +
                                                   std::to_string(group.configurable) + "; expected 0 or 1"};
        }
        // An uncategorized configurable group emits its toggle in a section named after the group.
        if (group.configurable && category.empty() && category_ids.contains(fc::detail::folded_id(id))) {
            return Diagnostic{candidate.owner, "Generated INI section for a group conflicts with another declaration"};
        }
        if (!has_valid_array_view(group.members, group.member_count, "group members", owner, error)) {
            return error;
        }

        for (std::uint32_t member_index = 0; member_index < group.member_count; ++member_index) {
            std::string_view member;
            if (!read_text_view(group.members[member_index], "group member", owner, member, error)) {
                return error;
            }
            const auto folded_member = fc::detail::folded_id(member);
            if (!patch_ids.contains(folded_member)) {
                return Diagnostic{candidate.owner,
                                  "group '" + std::string{id} + "' references unknown patch member '" + std::string{member} + "'"};
            }
            if (const auto [iterator, inserted] = memberships.emplace(folded_member, std::string{id}); !inserted) {
                return Diagnostic{candidate.owner, "patch '" + std::string{member} + "' belongs to more than one group ('" +
                                                       iterator->second + "' and '" + std::string{id} + "')"};
            }
        }
    }

    return std::nullopt;
}

} // namespace

AdmissionResult admit_definitions(std::span<const DefinitionCandidate> candidates) {
    AdmissionResult result;
    std::vector<const DefinitionCandidate *> validated;
    validated.reserve(candidates.size());
    std::unordered_map<std::string, std::vector<const DefinitionCandidate *>> candidates_by_id;

    // Group validated declarations by ID so structural errors and ID conflicts remain distinct.
    for (const auto &candidate : candidates) {
        if (auto error = validate_plugin_definition(candidate)) {
            result.diagnostics.push_back(std::move(*error));
            continue;
        }

        const auto id = fc::detail::view(candidate.definition->id);
        validated.push_back(&candidate);
        candidates_by_id[fc::detail::folded_id(id)].push_back(&candidate);
    }

    // Reject every plugin sharing an ID to make the result independent of file order.
    for (const auto &[folded_id, owners] : candidates_by_id) {
        if (owners.size() < 2) {
            continue;
        }

        for (const auto *candidate : owners) {
            std::string other_owners;
            for (const auto *other : owners) {
                if (other == candidate) {
                    continue;
                }
                if (!other_owners.empty()) {
                    other_owners += ", ";
                }
                other_owners += other->owner;
            }
            result.diagnostics.push_back(
                {candidate->owner, "plugin ID conflicts with " + other_owners + " (ASCII case-insensitive ID '" + folded_id + "')"});
        }
    }

    // Keep valid, nonconflicting declarations in their original candidate order.
    for (const auto *candidate : validated) {
        if (candidates_by_id.at(fc::detail::folded_id(fc::detail::view(candidate->definition->id))).size() == 1) {
            result.admitted.push_back({candidate->owner, candidate->definition});
        }
    }
    return result;
}

} // namespace fc::runtime
