#include "configuration.hpp"

#include "storage.hpp"

#include <fc/base.hpp>

#include <Windows.h>

#include <algorithm>
#include <map>
#include <unordered_set>
#include <utility>

namespace fc::runtime {

using namespace configuration_detail;

namespace {

// Copy a declaration default, taking ownership of text before it enters captured settings.
ScalarValue copy_value(const FC_Value &value) {
    switch (value.kind) {
    case FC_SCALAR_BOOL:
        return {value.value.boolean != 0};
    case FC_SCALAR_SIGNED:
        return {value.value.signed_value};
    case FC_SCALAR_UNSIGNED:
        return {value.value.unsigned_value};
    case FC_SCALAR_REAL:
        return {value.value.real};
    default:
        return {std::string{fc::detail::view(value.value.text)}};
    }
}

// Read one UTF-8-named environment variable; missing input returns nullopt and empty input remains a value.
std::optional<std::string> environment(std::string_view name) {
    if (name.empty()) {
        return {};
    }

    // Windows environment APIs use UTF-16 names and values.
    const auto wide_count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name.data(), static_cast<int>(name.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(wide_count), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name.data(), static_cast<int>(name.size()), wide.data(), wide_count);

    // A zero length can mean an absent variable or a present variable with an empty value.
    SetLastError(ERROR_SUCCESS);
    auto size = GetEnvironmentVariableW(wide.c_str(), nullptr, 0);
    if (!size) {
        return GetLastError() == ERROR_ENVVAR_NOT_FOUND ? std::nullopt : std::optional<std::string>{""};
    }

    std::wstring input(size, L'\0');
    auto received = GetEnvironmentVariableW(wide.c_str(), input.data(), size);
    // Return invalid UTF-8 when the value outgrows the buffer so scalar parsing selects the declared default.
    if (received >= size) {
        return std::string{"\xff"};
    }

    // Conversion failures use the same invalid-input marker rather than appearing as missing environment input.
    auto bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input.data(), static_cast<int>(received), nullptr, 0, nullptr, nullptr);
    if (!bytes && received) {
        return std::string{"\xff"};
    }

    std::string result(static_cast<std::size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input.data(), static_cast<int>(received), result.data(), bytes, nullptr, nullptr);
    return result;
}

} // namespace

std::expected<PluginConfiguration, Diagnostic> load_configuration(const AdmittedPlugin &admitted,
                                                                  std::span<const ResolvedPatch *const> patches, FC_Role role,
                                                                  std::span<const ImageObservation> observations,
                                                                  const ConfigurationPaths &paths) {
    PluginConfiguration configuration;
    const auto &plugin = *admitted.definition;
    const auto path = paths.directory / ("FC." + std::string{fc::detail::view(plugin.id)} + ".ini");
    const auto owner = path_text(path);
    std::vector<GeneratedEntry> entries;
    std::map<std::string, std::size_t> keys;
    std::vector<std::pair<const FC_Patch *, const FC_Variant *>> applicable;
    std::unordered_set<const FC_Patch *> eligible;

    // Merge keys shared by applicable variants, preserving the first declaration's presentation.
    auto add = [&](GeneratedEntry entry) {
        const auto [position, inserted] = keys.emplace(identity(entry.section, entry.key), entries.size());
        if (inserted) {
            entries.push_back(std::move(entry));
        } else {
            // Differing kinds or defaults require an unset generated key so the selected variant keeps its own default.
            auto &existing = entries[position->second];
            existing.variant_default |=
                existing.value.kind != entry.value.kind || format_scalar(existing.value) != format_scalar(entry.value);
        }
    };

    // Category toggles use the declaration's category spelling; uncategorized owners use [owner].Enabled.
    auto toggle = [&](FC_Text id, FC_Text category, FC_Text description, std::uint32_t enabled) {
        FC_Value value{};
        value.kind = FC_SCALAR_BOOL;
        value.value.boolean = enabled;
        std::string section = std::string{fc::detail::view(category)};
        if (!section.empty()) {
            for (std::uint32_t i = 0; i != plugin.category_count; ++i) {
                if (fc::detail::equal_id(fc::detail::view(plugin.categories[i].id), section)) {
                    section = fc::detail::view(plugin.categories[i].id);
                }
            }
        }
        add({section.empty() ? std::string{fc::detail::view(id)} : section, section.empty() ? "Enabled" : std::string{fc::detail::view(id)},
             std::string{fc::detail::view(description)}, "", value});
    };

    // Evaluate each already-expanded variant to include schemas whose known DLLs may arrive later.
    for (const auto *patch : patches) {
        if (patch->plugin != &plugin) {
            continue;
        }

        for (const auto &variant : patch->variants) {
            auto selection = evaluate_patch(*patch, role, observations, {&variant, 1});
            if (selection.state == SelectionState::Unavailable) {
                continue;
            }

            eligible.insert(patch->definition);
            applicable.emplace_back(patch->definition, variant.definition);

            // Variant settings live under the patch ID, optionally followed by the author's subsection name.
            for (std::uint32_t i = 0; i != variant.definition->setting_count; ++i) {
                const auto &setting = variant.definition->settings[i];
                auto section = std::string{fc::detail::view(patch->definition->id)};
                if (setting.section.size) {
                    section += "." + std::string{fc::detail::view(setting.section)};
                }
                add({section, std::string{fc::detail::view(setting.key)}, std::string{fc::detail::view(setting.description)},
                     std::string{fc::detail::view(setting.constraints)}, setting.default_value});
            }
        }

        if (eligible.contains(patch->definition) && patch->definition->configurable) {
            toggle(patch->definition->id, patch->definition->category, patch->definition->description, patch->definition->enabled);
        }
    }

    // Generate configurable group toggles only when the group contains a potentially applicable patch.
    for (std::uint32_t i = 0; i != plugin.group_count; ++i) {
        const auto &group = plugin.groups[i];
        if (!group.configurable) {
            continue;
        }

        bool relevant{};
        for (std::uint32_t j = 0; j != group.member_count; ++j) {
            for (const auto *patch : eligible) {
                relevant |= fc::detail::equal_id(fc::detail::view(patch->id), fc::detail::view(group.members[j]));
            }
        }
        if (relevant) {
            toggle(group.id, group.category, group.description, group.enabled);
        }
    }

    // Rank category sections before other sections, with explicitly ordered categories first.
    // Numeric category order and ASCII case-insensitive section name break ties within each category class.
    auto rank = [&](std::string_view section) {
        for (std::uint32_t i = 0; i != plugin.category_count; ++i) {
            const auto &category = plugin.categories[i];
            if (fc::detail::equal_id(fc::detail::view(category.id), section)) {
                return std::tuple{category.has_order ? 0u : 1u, category.order, fc::detail::folded_id(section)};
            }
        }
        return std::tuple{2u, 0u, fc::detail::folded_id(section)};
    };

    // Sort keys within category sections; other sections keep their keys in declaration order.
    if (paths.write_defaults) {
        for (auto &entry : entries) {
            entry.rank = rank(entry.section);
            entry.key_id = fc::detail::folded_id(entry.key);
        }
        std::stable_sort(entries.begin(), entries.end(), [](const auto &a, const auto &b) {
            if (a.rank != b.rank) {
                return a.rank < b.rank;
            }
            return std::get<0>(a.rank) < 2 && a.key_id < b.key_id;
        });
    }

    // Offline defaults use declaration schemas without consulting relative files in the working directory.
    std::expected<IniDocument, Diagnostic> document = paths.directory.empty() && !paths.write_defaults
                                                          ? std::expected<IniDocument, Diagnostic>{IniDocument{}}
                                                          : load_file(path, entries, paths.write_defaults, configuration.warnings);
    if (!document) {
        return std::unexpected(document.error());
    }

    auto parsed_document = std::move(*document);
    warn_unknown(parsed_document, entries, configuration.warnings, owner);

    std::vector<ControlOverride> overrides;
    // Read explicit toggles separately from scalar settings; invalid toggles leave declaration defaults active.
    auto control = [&](FC_Text id, FC_Text category, std::uint32_t configurable) {
        if (!configurable) {
            return;
        }

        auto section = category.size ? fc::detail::view(category) : fc::detail::view(id);
        auto key = category.size ? fc::detail::view(id) : std::string_view{"Enabled"};
        if (const auto *entry = find(parsed_document, section, key)) {
            auto value = parse_scalar(entry->value, FC_SCALAR_BOOL, owner);
            if (value) {
                overrides.push_back({fc::detail::view(id), std::get<bool>(value->value)});
            } else {
                configuration.warnings.push_back(
                    {owner, "Invalid toggle '" + std::string{fc::detail::view(id)} + "'; its default is used"});
            }
        }
    };

    for (std::uint32_t i = 0; i != plugin.patch_count; ++i) {
        control(plugin.patches[i].id, plugin.patches[i].category, plugin.patches[i].configurable);
    }
    for (std::uint32_t i = 0; i != plugin.group_count; ++i) {
        control(plugin.groups[i].id, plugin.groups[i].category, plugin.groups[i].configurable);
    }

    // Apply patch and group overrides together so each patch receives its effective enabled state.
    configuration.controls = resolve_controls(admitted, overrides);

    // Capture each ASCII case-insensitive environment name once so applicable variants share startup input.
    std::map<std::string, std::optional<std::string>> captured_environment;
    for (const auto &[patch, variant] : applicable) {
        CapturedVariantSettings captured{variant};
        for (std::uint32_t i = 0; i != variant->setting_count; ++i) {
            const auto &setting = variant->settings[i];
            auto section = std::string{fc::detail::view(patch->id)};
            if (setting.section.size) {
                section += "." + std::string{fc::detail::view(setting.section)};
            }

            const auto environment_name = std::string{fc::detail::view(setting.environment)};
            const auto environment_id = fc::detail::folded_id(environment_name);
            if (!captured_environment.contains(environment_id)) {
                captured_environment.emplace(environment_id, environment(environment_name));
            }
            const auto &env = captured_environment.at(environment_id);
            const auto *ini = find(parsed_document, section, fc::detail::view(setting.key));

            // Start with this variant's default; rejected environment or INI input leaves that default in place.
            auto value = copy_value(setting.default_value);

            // A present environment value wins even when empty or invalid; validation then decides default fallback.
            if (env || ini) {
                auto parsed = parse_scalar(env ? *env : ini->value, setting.default_value.kind, owner);
                if (parsed) {
                    const auto scalar = parsed->view();
                    Diagnostic rejected{owner, "Setting validation failed"};
                    // Copy the callback's borrowed error before the plugin resumes.
                    const auto capture = [](void *context, FC_Text message, std::uint32_t code) {
                        auto &diagnostic = *static_cast<Diagnostic *>(context);
                        if (message.size && message.data) {
                            diagnostic.message.assign(message.data, message.size);
                        }
                        diagnostic.windows_error = code;
                    };

                    // The admitted callback checks author constraints before the scalar replaces this variant's default.
                    if (setting.validate(setting.context, &scalar, {&rejected, capture}) == 1) {
                        value = std::move(*parsed);
                    } else {
                        parsed = std::unexpected(std::move(rejected));
                    }
                }

                // Attach the exact setting identity to conversion and validator failures for the warning report.
                if (!parsed) {
                    auto rejected = std::move(parsed.error());
                    rejected.message = "Invalid setting [" + section + "]." + std::string{fc::detail::view(setting.key)} + ": " +
                                       rejected.message + "; its default is used";
                    configuration.warnings.push_back(std::move(rejected));
                }
            }

            // Keep declaration order so the factory can assign each captured scalar to the matching field.
            captured.values.push_back(std::move(value));
        }
        configuration.variants.push_back(std::move(captured));
    }

    return configuration;
}

} // namespace fc::runtime
