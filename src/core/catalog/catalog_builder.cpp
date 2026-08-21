#include "catalog_builder.hpp"

#include "callback_error.hpp"
#include "configured_bundles.hpp"
#include "definition_copy.hpp"
#include "plugin_discovery.hpp"

#include "../config/configuration.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <exception>
#include <iterator>
#include <map>
#include <optional>
#include <ranges>
#include <string_view>
#include <utility>

namespace fc::catalog {
namespace {

using QueryPluginFn = const FC_PluginApi*(FC_CALL*)(std::uint32_t) noexcept;

// The submission failure is sticky, and submission_count enforces exactly one synchronous definition transfer.
struct SubmissionContext {
    std::optional<PluginDefinitionRecord> definition;
    std::optional<std::string> failure;
    AdmissionStage failure_stage{AdmissionStage::Registration};
    std::uint32_t submission_count{};
};

// AcquisitionOutcome keeps a rejected contribution's owned diagnostic beside an optional provisional survivor.
struct AcquisitionOutcome {
    std::optional<ProvisionalPlugin> plugin;
    RejectionRecord rejection;
};

// Accepts one synchronous definition and severs every dependency on the plugin's temporary pointer tree.
FC_SubmitResult FC_CALL submit_definition(void* context, const FC_PluginDefinition* plugin) {
    auto& submission = *static_cast<SubmissionContext*>(context);
    ++submission.submission_count;
    if (submission.submission_count != 1) {
        submission.failure = "Registration submitted more than one plugin definition";
        submission.failure_stage = AdmissionStage::Registration;
        return FC_SUBMIT_REJECTED;
    }
    try {
        auto copied = copy_plugin_definition(plugin);
        if (!copied) {
            submission.failure = std::move(copied.error());
            submission.failure_stage = AdmissionStage::Copying;
            return FC_SUBMIT_REJECTED;
        }
        submission.definition = std::move(*copied);
        return FC_SUBMIT_ACCEPTED;
    } catch (const std::exception& exception) {
        submission.failure = std::string{"Copying the plugin definition failed: "} + exception.what();
    } catch (...) {
        submission.failure = "Copying the plugin definition failed with an unknown exception";
    }
    submission.failure_stage = AdmissionStage::Copying;
    return FC_SUBMIT_REJECTED;
}

// Converts a provisional failure into the stable attribution retained after its code and registration owners unwind.
[[nodiscard]] RejectionRecord rejection_for(const ProvisionalPlugin& plugin, AdmissionStage stage, std::string reason) {
    RejectionRecord result{.plugin_id = plugin.definition.id, .stage = stage, .reason = std::move(reason)};
    if (plugin.library) {
        result.path = plugin.library->path();
    }
    return result;
}

// Gives every source of contributions the same boundary for exceptions, diagnostic reporting, and ownership transfer.
[[nodiscard]] AcquisitionOutcome invoke_registration(PluginOrigin origin, std::size_t order, std::uint32_t sdk_revision,
                                                     FC_RegisterPluginFn registration,
                                                     RegistrationOwner registration_owner,
                                                     std::optional<NativeLibrary> library, const CodeOwner& code_owner,
                                                     const FC_HostApi& host,
                                                     std::optional<std::filesystem::path> path = std::nullopt) {
    AcquisitionOutcome outcome;
    outcome.rejection.path = std::move(path);
    outcome.rejection.stage = AdmissionStage::Registration;
    if (registration == nullptr || !code_owner.contains_executable(reinterpret_cast<std::uintptr_t>(registration))) {
        outcome.rejection.reason = "The registration callback is null or outside its executable code owner";
        return outcome;
    }

    // Sinks owned by this stack frame constrain all plugin output to this one synchronous registration invocation.
    SubmissionContext submission;
    CallbackError callback_error{.malformed_fallback = "The registration callback supplied malformed error text",
                                 .failure_fallback = "The registration callback failed"};
    const FC_RegistrySink registry{
        .struct_size = sizeof(FC_RegistrySink), .context = &submission, .submit = &submit_definition};
    const auto error = callback_error.sink();

    FC_CallStatus status = FC_CALL_FAILED;
    try {
        status = registration(&host, &registry, &error);
    } catch (...) {
        outcome.rejection.reason = "The registration callback threw across the native boundary";
        return outcome;
    }

    // Reconcile callback status with sink activity; neither success signal can compensate for the other's failure.
    if (submission.definition) {
        outcome.rejection.plugin_id = submission.definition->id;
    }
    if (submission.failure) {
        outcome.rejection.stage = submission.failure_stage;
        outcome.rejection.reason = std::move(*submission.failure);
        return outcome;
    }
    if (status != FC_CALL_OK) {
        outcome.rejection.reason = callback_error.supplied && !callback_error.message.empty()
                                       ? std::move(callback_error.message)
                                       : "The registration callback failed";
        outcome.rejection.operation = std::move(callback_error.operation);
        return outcome;
    }
    if (submission.submission_count != 1 || !submission.definition) {
        outcome.rejection.reason = "Successful registration did not submit exactly one accepted definition";
        return outcome;
    }

    // Transfer owners only after the callback and copied definition have jointly passed the registration contract.
    ProvisionalPlugin plugin;
    plugin.origin = origin;
    plugin.acquisition_order = order;
    plugin.sdk_revision = sdk_revision;
    plugin.registration_owner = std::move(registration_owner);
    plugin.library = std::move(library);
    plugin.code_owner = code_owner;
    plugin.definition = std::move(*submission.definition);
    outcome.plugin = std::move(plugin);
    return outcome;
}

[[nodiscard]] AcquisitionOutcome acquire_bridge(PluginOrigin origin, std::size_t order, RegistrationBridge bridge,
                                                const CodeOwner& code_owner, const FC_HostApi& host) {
    return invoke_registration(origin, order, FC_SDK_REVISION, bridge.register_plugin,
                               RegistrationOwner{bridge.release}, std::nullopt, code_owner, host);
}

// Loads a pre-inspected external candidate and validates its retained query table before registration executes.
[[nodiscard]] AcquisitionOutcome acquire_external(ExternalCandidate candidate, const FC_HostApi& host) {
    AcquisitionOutcome outcome;
    outcome.rejection.path = candidate.path;
    outcome.rejection.stage = AdmissionStage::Loading;
    auto library = NativeLibrary::load(candidate.path);
    if (!library) {
        outcome.rejection.reason = std::move(library.error());
        return outcome;
    }

    // The query itself must be executable within the newly owned image before any call controlled by the plugin occurs.
    const auto query_address = library->find_export("FusionCutter_QueryPlugin");
    if (query_address == nullptr ||
        !library->code_owner().contains_executable(reinterpret_cast<std::uintptr_t>(query_address))) {
        outcome.rejection.stage = AdmissionStage::Query;
        outcome.rejection.reason = "The retained plugin query export is missing or non-executable";
        return outcome;
    }
    const auto query = reinterpret_cast<QueryPluginFn>(query_address);
    const FC_PluginApi* supplied_api{};
    try {
        supplied_api = query(FC_PLUGIN_ABI_GENERATION);
    } catch (...) {
        outcome.rejection.stage = AdmissionStage::Query;
        outcome.rejection.reason = "The plugin query threw across the native boundary";
        return outcome;
    }
    if (supplied_api == nullptr) {
        outcome.rejection.stage = AdmissionStage::Query;
        outcome.rejection.reason = "The plugin does not support the requested ABI generation";
        return outcome;
    }
    if (!library->code_owner().contains_image_range(supplied_api, sizeof(std::uint32_t))) {
        outcome.rejection.stage = AdmissionStage::Query;
        outcome.rejection.reason = "The plugin API table is outside its owning image";
        return outcome;
    }

    // Copies only the ABI generation 1 prefix proven to reside in the retained module.
    std::uint32_t supplied_size{};
    std::memcpy(&supplied_size, supplied_api, sizeof(supplied_size));
    constexpr auto required_size = offsetof(FC_PluginApi, register_plugin) + sizeof(FC_RegisterPluginFn);
    if (supplied_size < required_size || !library->code_owner().contains_image_range(supplied_api, required_size)) {
        outcome.rejection.stage = AdmissionStage::Query;
        outcome.rejection.reason = "The plugin API table does not reach the prefix required by ABI generation 1";
        return outcome;
    }

    FC_PluginApi api{};
    // Zero initialization supplies absent tail defaults while the bounded copy preserves a larger future table.
    std::memcpy(&api, supplied_api, std::min<std::size_t>(sizeof(api), supplied_size));
    if (api.host_api_size > host.struct_size) {
        outcome.rejection.stage = AdmissionStage::Query;
        outcome.rejection.reason = "The plugin requires a larger host API table";
        return outcome;
    }
    if (api.register_plugin == nullptr ||
        !library->code_owner().contains_executable(reinterpret_cast<std::uintptr_t>(api.register_plugin))) {
        outcome.rejection.stage = AdmissionStage::Query;
        outcome.rejection.reason = "The plugin registration entry is null or outside its executable image";
        return outcome;
    }

    const auto code_owner = library->code_owner();
    return invoke_registration(PluginOrigin::External, candidate.discovery_order, api.sdk_revision, api.register_plugin,
                               RegistrationOwner{}, std::optional<NativeLibrary>{std::move(*library)}, code_owner, host,
                               candidate.path);
}

// Bundled plugins use ID order instead of registration order so admission is reproducible across link layouts.
[[nodiscard]] bool bundle_order(const ProvisionalPlugin& left, const ProvisionalPlugin& right) {
    const auto left_folded = fold_ascii(left.definition.id);
    const auto right_folded = fold_ascii(right.definition.id);
    if (left_folded != right_folded) {
        return left_folded < right_folded;
    }
    return left.definition.id < right.definition.id;
}

// Marks the complete set so removing one loser cannot rescue another; only the built-in Core plugin wins a collision.
void mark_collisions(const std::vector<ProvisionalPlugin>& plugins, std::vector<std::optional<std::string>>& reasons) {
    struct CollisionBucket {
        std::string declared_spelling;
        std::vector<std::size_t> participants;
    };
    // Collect plugin IDs separately while patches and groups share the one global definition namespace.
    std::map<std::string, CollisionBucket> plugin_ids;
    std::map<std::string, CollisionBucket> definition_ids;
    const auto add = [](auto& values, std::string_view id, std::size_t index) {
        auto& bucket = values[fold_ascii(id)];
        if (bucket.declared_spelling.empty()) {
            bucket.declared_spelling = id;
        }
        bucket.participants.push_back(index);
    };
    for (std::size_t index = 0; index < plugins.size(); ++index) {
        add(plugin_ids, plugins[index].definition.id, index);
        for (const auto& patch : plugins[index].definition.patches) {
            add(definition_ids, patch.id, index);
        }
        for (const auto& group : plugins[index].definition.groups) {
            add(definition_ids, group.id, index);
        }
    }

    // Mark every participant from frozen buckets, except that the built-in Core plugin survives its own collision.
    const auto mark_map = [&](const auto& values, std::string_view kind) {
        for (const auto& [folded_id, bucket] : values) {
            (void)folded_id;
            const auto& participants = bucket.participants;
            if (participants.size() < 2) {
                continue;
            }
            const auto core = std::ranges::find_if(participants, [&](std::size_t index) {
                return plugins[index].origin == PluginOrigin::Core;
            });
            for (const auto index : participants) {
                if (core != participants.end() && plugins[index].origin == PluginOrigin::Core) {
                    continue;
                }
                if (!reasons[index]) {
                    reasons[index] = std::string{kind} + " collision on '" + bucket.declared_spelling + "'";
                }
            }
        }
    };
    mark_map(plugin_ids, "Plugin ID");
    mark_map(definition_ids, "Patch/group ID");
}

// Selects at most one support ordinal after structural admission has proved expanded target masks cannot overlap.
void match_supports(ProvisionalPlugin& plugin, const targets::RecognizedTarget& target) noexcept {
    for (auto& patch : plugin.definition.patches) {
        patch.selected_support.reset();
        for (std::size_t index = 0; index < patch.supports.size(); ++index) {
            const auto& support = patch.supports[index];
            if ((support.layouts & target.layout()) != 0 && (support.roles & target.role()) != 0) {
                patch.selected_support = static_cast<std::uint32_t>(index);
                break;
            }
        }
    }
}

} // namespace

CatalogBuilder::CatalogBuilder(const FC_HostApi& host, CodeOwner core_code_owner)
    : host_(&host), core_code_owner_(std::move(core_code_owner)) {}

void CatalogBuilder::add_core(RegistrationBridge contribution) {
    core_ = contribution;
}

void CatalogBuilder::add_bundled(RegistrationBridge contribution) {
    bundles_.push_back(contribution);
}

void CatalogBuilder::set_external_discovery(DiscoveryResult discovery) {
    external_discovery_ = std::move(discovery);
}

CatalogBuildResult CatalogBuilder::build(const targets::RecognizedTarget& target,
                                         const config::ConfigurationPaths& paths, CatalogBuildObserver observer) {
    CatalogBuildResult result;
    if (built_) {
        result.fatal_error = "CatalogBuilder::build may be called only once";
        return result;
    }
    built_ = true;
    if (core_.register_plugin == nullptr) {
        result.fatal_error = "The built-in Core plugin contribution bridge is missing";
        return result;
    }

    // Acquisition executes each origin in policy order while retaining every accepted contribution provisionally.
    auto core = acquire_bridge(PluginOrigin::Core, 0, core_, core_code_owner_, *host_);
    if (!core.plugin) {
        result.fatal_error = core.rejection.reason;
        return result;
    }

    std::vector<ProvisionalPlugin> bundled;
    bundled.reserve(bundles_.size());
    for (std::size_t index = 0; index < bundles_.size(); ++index) {
        auto candidate = acquire_bridge(PluginOrigin::Bundled, index, bundles_[index], core_code_owner_, *host_);
        if (candidate.plugin) {
            bundled.push_back(std::move(*candidate.plugin));
        } else {
            result.rejections.push_back(std::move(candidate.rejection));
        }
    }
    std::ranges::sort(bundled, bundle_order);

    auto discovery = external_discovery_ ? std::move(*external_discovery_)
                                         : discover_plugins(paths.installation_directory / "plugins");
    result.rejections.insert(result.rejections.end(), std::make_move_iterator(discovery.rejections.begin()),
                             std::make_move_iterator(discovery.rejections.end()));
    std::vector<ProvisionalPlugin> external;
    external.reserve(discovery.candidates.size());
    for (auto& candidate : discovery.candidates) {
        auto acquired = acquire_external(std::move(candidate), *host_);
        if (acquired.plugin) {
            external.push_back(std::move(*acquired.plugin));
        } else {
            result.rejections.push_back(std::move(acquired.rejection));
        }
    }

    // The unified sequence fixes the built-in Core plugin, sorted bundled plugins, then discovered external plugins.
    std::vector<ProvisionalPlugin> acquired;
    acquired.reserve(1 + bundled.size() + external.size());
    acquired.push_back(std::move(*core.plugin));
    for (auto& plugin : bundled) {
        acquired.push_back(std::move(plugin));
    }
    for (auto& plugin : external) {
        acquired.push_back(std::move(plugin));
    }

    // Structural validation runs while callback code and registration state are still owned by each provisional record.
    std::vector<ProvisionalPlugin> structurally_valid;
    structurally_valid.reserve(acquired.size());
    for (auto& plugin : acquired) {
        const auto& owner = plugin.library ? plugin.library->code_owner() : core_code_owner_;
        auto valid = validate_plugin_definition(plugin.definition, plugin.origin, owner);
        if (!valid) {
            if (plugin.origin == PluginOrigin::Core) {
                result.fatal_error = std::move(valid.error());
                return result;
            }
            result.rejections.push_back(rejection_for(plugin, AdmissionStage::Structure, std::move(valid.error())));
            continue;
        }
        structurally_valid.push_back(std::move(plugin));
    }

    std::vector<std::optional<std::string>> collision_reasons(structurally_valid.size());
    // Collision results are frozen before any loser is removed; capacity then advances once without backtracking.
    mark_collisions(structurally_valid, collision_reasons);
    std::vector<ProvisionalPlugin> collision_survivors;
    collision_survivors.reserve(structurally_valid.size());
    for (std::size_t index = 0; index < structurally_valid.size(); ++index) {
        if (collision_reasons[index]) {
            result.rejections.push_back(rejection_for(structurally_valid[index], AdmissionStage::Collision,
                                                      std::move(*collision_reasons[index])));
        } else {
            collision_survivors.push_back(std::move(structurally_valid[index]));
        }
    }

    // Capacity is a single forward pass; rejected entries do not cause earlier gates or ordering to be reconsidered.
    std::vector<ProvisionalPlugin> capacity_survivors;
    capacity_survivors.reserve(collision_survivors.size());
    std::size_t definition_count{};
    for (auto& plugin : collision_survivors) {
        const auto contribution_count = plugin.definition.definition_count();
        if (plugin.origin == PluginOrigin::Core && contribution_count > kGlobalDefinitionCapacity) {
            result.fatal_error = "The built-in Core plugin exceeds the global definition capacity";
            return result;
        }
        if (contribution_count > kGlobalDefinitionCapacity - definition_count) {
            result.rejections.push_back(rejection_for(
                plugin, AdmissionStage::Capacity, "The contribution exceeds the remaining global definition capacity"));
            continue;
        }
        definition_count += contribution_count;
        capacity_survivors.push_back(std::move(plugin));
    }

    // Support selection and configuration form the final gate before process-lifetime ownership is transferred.
    if (observer.begin_configuration != nullptr) {
        observer.begin_configuration(observer.context);
    }
    config::ConfigurationSnapshot configuration;
    std::vector<PluginRecord> final_plugins;
    final_plugins.reserve(capacity_survivors.size());
    configuration.plugins.reserve(capacity_survivors.size());
    for (auto& plugin : capacity_survivors) {
        match_supports(plugin, target);
        const bool core_plugin = plugin.origin == PluginOrigin::Core;
        auto loaded = config::load_configuration(plugin.definition, core_plugin, paths);
        if (!loaded) {
            if (core_plugin) {
                result.fatal_error = std::move(loaded.error());
                return result;
            }
            auto rejection = rejection_for(plugin, AdmissionStage::Configuration, std::move(loaded.error()));
            rejection.operation = "Load plugin configuration";
            result.rejections.push_back(std::move(rejection));
            continue;
        }
        if (loaded->framework) {
            configuration.framework = *loaded->framework;
        }
        configuration.plugins.push_back(std::move(loaded->configuration));

        // Only fully configured survivors shed provisional policy state and enter the immutable plugin catalog.
        PluginRecord final;
        final.sdk_revision = plugin.sdk_revision;
        final.registration_owner = std::move(plugin.registration_owner);
        final.library = std::move(plugin.library);
        final.code_owner = std::move(plugin.code_owner);
        final.definition = std::move(plugin.definition);
        final_plugins.push_back(std::move(final));
    }

    result.catalog.emplace(std::move(final_plugins));
    result.configuration.emplace(std::move(configuration));
    return result;
}

CatalogBuildResult acquire_catalog(const FC_HostApi& host, const targets::RecognizedTarget& target,
                                   const std::filesystem::path& installation_directory, CatalogBuildObserver observer) {
    // FusionCutter.dll has process lifetime, so CodeOwner observes it without acquiring another module handle.
    auto core_owner = CodeOwner::from_address(reinterpret_cast<std::uintptr_t>(&acquire_catalog));
    if (!core_owner) {
        CatalogBuildResult result;
        result.fatal_error = std::move(core_owner.error());
        return result;
    }

    CatalogBuilder builder{host, std::move(*core_owner)};
    builder.add_core(core_registration_bridge());
    for (const auto& bundle : configured_bundle_bridges()) {
        builder.add_bundled(bundle);
    }
    return builder.build(target, config::ConfigurationPaths{installation_directory}, observer);
}

CatalogBuildResult acquire_catalog_explicit(const FC_HostApi& host, const targets::RecognizedTarget& target,
                                            const std::filesystem::path& installation_directory,
                                            std::span<const std::filesystem::path> plugin_paths,
                                            CatalogBuildObserver observer) {
    // Reuse code owners for the framework and bundled plugins; explicit discovery replaces directory enumeration.
    auto core_owner = CodeOwner::from_address(reinterpret_cast<std::uintptr_t>(&acquire_catalog_explicit));
    if (!core_owner) {
        CatalogBuildResult result;
        result.fatal_error = std::move(core_owner.error());
        return result;
    }
    CatalogBuilder builder{host, std::move(*core_owner)};
    builder.add_core(core_registration_bridge());
    for (const auto& bundle : configured_bundle_bridges()) {
        builder.add_bundled(bundle);
    }
    builder.set_external_discovery(discover_plugin_paths(plugin_paths));
    return builder.build(target, config::ConfigurationPaths{installation_directory}, observer);
}

} // namespace fc::catalog
