#include "offline_validation.hpp"

#include "patches/installation.hpp"
#include "fatal_boundary.hpp"
#include "../../plugins/core.hpp"

#include <algorithm>
#include <exception>
#include <fstream>
#include <limits>
#include <ranges>
#include <system_error>

namespace fc::validation {

namespace {

// Converts a diagnostic to an error while retaining its owner and Windows error code.
fc::Error error(const runtime::Diagnostic &value) {
    return {value.owner + ": " + value.message, value.windows_error};
}

// Reads the complete binary file; open, size and short-read failures return an error.
fc::Result<std::vector<std::byte>> read_file(const std::filesystem::path &path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        return std::unexpected(fc::Error{runtime::path_text(path) + ": could not open image file"});
    }

    const auto extent = file.tellg();
    if (extent < 0 || static_cast<std::uintmax_t>(extent) > std::numeric_limits<std::size_t>::max()) {
        return std::unexpected(
            fc::Error{runtime::path_text(path) + ": could not read the image file size, or it exceeds this process's size limit"});
    }

    std::vector<std::byte> bytes(static_cast<std::size_t>(extent));
    file.seekg(0);
    if (!bytes.empty() && !file.read(reinterpret_cast<char *>(bytes.data()), extent)) {
        return std::unexpected(fc::Error{runtime::path_text(path) + ": could not read complete image file"});
    }

    return bytes;
}

// Records the failure, releases the unexposed handler and rejects dependents; retains the plan for reporting.
void reject(runtime::SelectionSession &session, runtime::PatchRecord &patch, fc::Error value) {
    patch.state = runtime::PatchState::Failed;
    patch.reason = value.message;
    session.diagnostics.push_back({runtime::qualified_patch_id(patch), std::move(value.message), value.windows_error});
    patch.destroy_unexposed();
    runtime::resolve_dependencies(session);
}

} // namespace

const runtime::PatchRecord *find_patch(const ValidationResult &result, std::string_view id) noexcept {
    const auto separator = id.find('.');
    for (const auto &patch : result.session->patches) {
        const auto plugin = fc::detail::view(patch->plugin->admitted.definition->id), local = fc::detail::view(patch->definition->id);
        if (separator != std::string_view::npos && fc::detail::equal_id(plugin, id.substr(0, separator)) &&
            fc::detail::equal_id(local, id.substr(separator + 1))) {
            return patch.get();
        }
    }

    return nullptr;
}

fc::Result<ValidationResult> validate_offline(const ValidationInput &input) {
    if (input.role != FC_ROLE_CLIENT && input.role != FC_ROLE_SERVER) {
        return std::unexpected(fc::Error{"Validation role must be Client or Server"});
    }

    // Supplied plugins must be accessible files before DLL admission can assess their declarations.
    for (const auto &path : input.plugins) {
        std::error_code failure;
        const auto regular = std::filesystem::is_regular_file(path, failure);
        if (!regular || failure) {
            return std::unexpected(fc::Error{runtime::path_text(path) + ": supplied plugin path does not name an accessible regular file",
                                             static_cast<std::uint32_t>(failure.value())});
        }
    }

    const auto catalog = runtime::known_image_profiles();
    ValidationResult result;

    // Read runtime settings with the production parser in its read-only mode.
    if (input.config_directory && !input.config_directory->empty()) {
        auto settings = runtime::load_runtime_configuration({*input.config_directory, false});
        if (!settings) {
            return std::unexpected(error(settings.error()));
        }
        result.runtime_settings = std::move(*settings);
    }

    std::vector<runtime::ImageObservation> observations;
    // Recognize each file before laying out its bytes as they would appear in a loaded image.
    for (const auto &source : input.images) {
        auto bytes = read_file(source.path);
        if (!bytes) {
            return std::unexpected(bytes.error());
        }

        const auto filename = runtime::path_text(source.path.filename());
        if (!source.profile.empty()) {
            auto requested = runtime::find_image_profile(source.profile, catalog);
            if (!requested) {
                return std::unexpected(error(requested.error()));
            }
        }

        if (std::ranges::any_of(observations, [&](const auto &previous) {
                return fc::detail::equal_id(previous.filename, filename);
            })) {
            return std::unexpected(fc::Error{filename + ": image filename was supplied more than once"});
        }

        auto layout = runtime::parse_file_image(*bytes, filename);
        if (!layout) {
            return std::unexpected(error(layout.error()));
        }
        auto profile = runtime::recognize_image(filename, *layout, catalog);
        if (!profile) {
            return std::unexpected(error(profile.error()));
        }
        if (!source.profile.empty() && !fc::detail::equal_id(source.profile, (*profile)->id)) {
            return std::unexpected(fc::Error{filename + ": recognized image profile does not match the requested profile"});
        }

        observations.push_back({filename, std::optional{*profile}});
        runtime::FileImage image{*profile, filename, std::move(*layout)};
        auto mapped = runtime::map_file_image(image, *bytes);
        if (!mapped) {
            return std::unexpected(error(mapped.error()));
        }
        result.images.push_back({std::move(image), std::move(*mapped)});
    }

    if (result.images.empty()) {
        return std::unexpected(fc::Error{"Validation requires at least one image"});
    }

    // Mark every unsupplied catalog filename as absent so selection can distinguish waiting patches.
    // Supplied images keep their original indices for the plan bindings below.
    for (const auto &profile : catalog) {
        for (const auto filename : profile.filenames) {
            if (!std::ranges::any_of(observations, [&](const auto &previous) {
                    return fc::detail::equal_id(previous.filename, filename);
                })) {
                observations.push_back({std::string{filename}, std::optional<const runtime::ImageProfile *>{}});
            }
        }
    }

    // Load the built-in plugin and requested DLLs, then apply configuration and select their variants.
    const runtime::DefinitionCandidate core{"Built-in Core", core_plugin::core_plugin()};
    auto admitted = runtime::load_plugins(input.plugins, std::span{&core, 1});
    result.session = std::make_unique<runtime::SelectionSession>(runtime::resolve_patches(std::move(admitted), catalog, input.role));
    runtime::configure_patches(*result.session, observations, {input.config_directory.value_or(std::filesystem::path{}), false});

    // Supply native resource callbacks for plans; live runtime services are unavailable during offline validation.
    result.host = std::make_unique<FC_Host>();
    result.host->byte_size = sizeof(FC_Host);
    result.host->native = runtime::native_api();

    auto ordered = runtime::ready_patch_order(*result.session);
    std::vector<runtime::SubmittedPlan *> plans;

    // Collect plans in dependency order so validation can check conflicts between all ready patches.
    for (auto *patch : ordered) {
        // An earlier constructor or Plan failure may already have rejected this dependent.
        if (patch->state != runtime::PatchState::Planning) {
            continue;
        }
        try {
            // Bind each selected image label to the corresponding mapped file buffer.
            std::vector<runtime::PlanImage> selected;
            for (const auto &binding : patch->selection.images) {
                const auto &image = result.images.at(binding.observation_index);
                selected.push_back(runtime::mapped_plan_image(std::string{binding.label}, image.image, image.mapped));
            }

            if (auto planned = runtime::collect_patch_plan(*patch, result.host.get(), std::move(selected)); !planned) {
                reject(*result.session, *patch, {planned.error().message, planned.error().windows_error});
                continue;
            }

            plans.push_back(patch->plan.get());
        } catch (const std::exception &value) {
            reject(*result.session, *patch, {value.what()});
        } catch (...) {
            reject(*result.session, *patch, {"Patch planning raised an unknown exception"});
        }
    }

    // Validate operations against the file bytes and reject plans whose requirements conflict.
    for (auto &[plan, failure] : runtime::validate_plans(plans, {})) {
        reject(*result.session, *plan->owner(), std::move(failure));
    }

    // A conflict can fail a provider after dependents planned; release their unexposed handlers as well.
    for (const auto &patch : result.session->patches) {
        if (patch->state == runtime::PatchState::Failed) {
            patch->destroy_unexposed();
        }
    }

    return result;
}

} // namespace fc::validation
