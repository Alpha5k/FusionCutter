#include "offline_validation.hpp"
#include "verify_report.hpp"

#include <Windows.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <ranges>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using fc::runtime::path_text;

// Collects command-line inputs for validation and any patches explicitly required to pass.
struct Arguments {
    bool dedicated{};
    std::vector<fc::validation::ImageInput> images;
    std::vector<std::filesystem::path> plugins;
    std::optional<std::filesystem::path> configuration;
    std::optional<std::filesystem::path> json;
    std::vector<std::string> checks;
};

// Compares Windows path text case-insensitively using ordinal Unicode comparison.
bool same_text(std::wstring_view left, std::wstring_view right) noexcept {
    return CompareStringOrdinal(left.data(), static_cast<int>(left.size()), right.data(), static_cast<int>(right.size()), TRUE) ==
           CSTR_EQUAL;
}

// Converts UTF-16 command-line text to UTF-8, returning an error if Windows cannot encode it.
std::expected<std::string, std::string> utf8(std::wstring_view value) {
    // Ask Windows for the encoded byte count before allocating the UTF-8 string.
    const auto required =
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (required <= 0) {
        return std::unexpected("Could not convert argument text to UTF-8");
    }

    std::string result(static_cast<std::size_t>(required), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), required, nullptr,
                        nullptr);
    return result;
}

// Parses verifier options and requires at least one profile-qualified image; invalid input returns an error.
std::expected<Arguments, std::string> parse_arguments(int count, wchar_t *values[]) {
    Arguments result;
    for (int index = 1; index < count; ++index) {
        const std::wstring_view option{values[index]};
        if (option == L"--dedicated") {
            result.dedicated = true;
            continue;
        }

        // The remaining options consume a value; convert option names for error messages.
        const auto option_text = utf8(option);
        if (!option_text) {
            return std::unexpected(option_text.error());
        }
        if (option != L"--image" && option != L"--plugin" && option != L"--config" && option != L"--check" && option != L"--json") {
            return std::unexpected("Unknown verifier option: " + *option_text);
        }
        if (index + 1 >= count) {
            return std::unexpected(*option_text + " requires a value");
        }

        const std::wstring_view value{values[++index]};
        if (value.empty()) {
            return std::unexpected(*option_text + " requires a nonempty value");
        }

        if (option == L"--image") {
            // Split only at the first '=' so the path can contain additional equals signs.
            const auto separator = value.find(L'=');
            if (separator == std::wstring_view::npos || !separator || separator + 1 == value.size()) {
                return std::unexpected("--image must use ProfileId=Path");
            }
            auto profile = utf8(value.substr(0, separator));
            if (!profile) {
                return std::unexpected(profile.error());
            }
            result.images.push_back({std::filesystem::path{value.substr(separator + 1)}, std::move(*profile)});
        } else if (option == L"--plugin") {
            result.plugins.emplace_back(value);
        } else if (option == L"--config") {
            if (result.configuration) {
                return std::unexpected("--config may be supplied only once");
            }
            result.configuration.emplace(value);
        } else if (option == L"--check") {
            auto id = utf8(value);
            if (!id) {
                return std::unexpected(id.error());
            }
            result.checks.push_back(std::move(*id));
        } else if (option == L"--json") {
            if (result.json) {
                return std::unexpected("--json may be supplied only once");
            }
            result.json.emplace(value);
        }
    }

    if (result.images.empty()) {
        return std::unexpected("At least one --image is required");
    }
    return result;
}

// Returns the display name for a patch's offline verification state.
std::string_view verification_state_name(fc::runtime::PatchState state) noexcept {
    // A successfully validated patch remains in Planning until installation; display it as Ready here.
    switch (state) {
    case fc::runtime::PatchState::Disabled:
        return "Disabled";
    case fc::runtime::PatchState::Waiting:
        return "Waiting";
    case fc::runtime::PatchState::Planning:
        return "Ready";
    case fc::runtime::PatchState::Failed:
    default:
        return "Failed";
    }
}

// Compares absolute path text and existing file identities, including symbolic links and hard links.
bool same_file(const std::filesystem::path &left, const std::filesystem::path &right) {
    if (same_text(std::filesystem::absolute(left).lexically_normal().native(),
                  std::filesystem::absolute(right).lexically_normal().native())) {
        return true;
    }
    std::error_code ignored;
    return std::filesystem::equivalent(left, right, ignored);
}

// Rejects report destinations that would overwrite a supplied image, plugin or configuration file.
std::optional<std::string> output_error(const Arguments &arguments) {
    if (!arguments.json) {
        return {};
    }
    for (const auto &image : arguments.images) {
        if (same_file(*arguments.json, image.path)) {
            return "--json would overwrite an image input";
        }
    }
    for (const auto &plugin : arguments.plugins) {
        if (same_file(*arguments.json, plugin)) {
            return "--json would overwrite a plugin input";
        }
    }
    if (arguments.configuration) {
        const auto output = std::filesystem::absolute(*arguments.json).lexically_normal();
        const auto directory = std::filesystem::absolute(*arguments.configuration).lexically_normal();
        if (same_file(output.parent_path(), directory) && same_text(output.extension().native(), L".ini")) {
            return "--json would overwrite a configuration input";
        }
        // Existing configuration files may also be reached through hard links outside the directory.
        std::error_code error;
        if (std::filesystem::is_directory(directory, error)) {
            for (const auto &entry : std::filesystem::directory_iterator(directory)) {
                if (entry.is_regular_file() && same_text(entry.path().extension().native(), L".ini") && same_file(output, entry.path())) {
                    return "--json would overwrite a configuration input";
                }
            }
        }
    }
    return {};
}

// Serializes retained native evidence as hex bytes, a mask or an expected image target.
std::string evidence_text(const fc::runtime::EvidenceRecord &evidence, const fc::runtime::SubmittedPlan &plan) {
    std::ostringstream text;
    if (!evidence.bytes.empty()) {
        text << "bytes=";
        for (const auto byte : evidence.bytes) {
            text << std::hex << std::setw(2) << std::setfill('0') << std::to_integer<unsigned>(byte);
        }
    }
    if (!evidence.mask.empty()) {
        text << " mask=";
        for (const auto byte : evidence.mask) {
            text << std::hex << std::setw(2) << std::setfill('0') << std::to_integer<unsigned>(byte);
        }
    }
    if (evidence.target) {
        text << "target=" << plan.images().at(evidence.target->image).profile << "+0x" << std::hex << evidence.target->rva;
    }
    return text.str();
}

// Collects production validation results and coverage gaps for readable and JSON output.
fc::verify::Report make_report(const Arguments &arguments, const fc::validation::ValidationResult &result) {
    using fc::verify::Status;
    fc::verify::Report report{.role = arguments.dedicated ? "Server" : "Client"};
    for (std::size_t index = 0; index < result.images.size(); ++index) {
        const auto &image = result.images[index].image;
        report.images.push_back({path_text(arguments.images.at(index).path), std::string{image.profile->id}, image.filename,
                                 image.layout.image_size, image.layout.preferred_base});
        report.checks.push_back(
            {Status::Passed, std::string{image.profile->id}, "Image recognition", "PE headers match the production profile"});
    }

    // Preserve accepted declarations and settings warnings; absent INI files are coverage gaps.
    for (const auto &plugin : result.session->plugins) {
        const std::string id{fc::detail::view(plugin->admitted.definition->id)};
        report.checks.push_back(
            {plugin->failure ? Status::Failed : Status::Passed,
             id,
             "Plugin admission and configuration",
             plugin->failure ? plugin->failure->message : "Plugin declarations and settings were accepted",
             {},
             "version=" + std::string{fc::detail::view(plugin->admitted.definition->version)} + " source=" + plugin->admitted.owner});
        if (plugin->configuration) {
            for (const auto &warning : plugin->configuration->warnings) {
                report.checks.push_back({Status::Passed, id, "Configuration warning", warning.message});
            }
        }
        const auto file = arguments.configuration.value_or(std::filesystem::path{}) / ("FC." + id + ".ini");
        std::error_code error;
        if (!arguments.configuration || !std::filesystem::exists(file, error)) {
            report.checks.push_back({Status::NotChecked, id, "Configuration file",
                                     "No configuration file was supplied or found; defaults and environment overrides apply"});
        }
    }
    for (const auto &warning : result.runtime_settings.warnings) {
        report.checks.push_back({Status::Passed, "FusionCutter", "Configuration warning", warning.message});
    }
    const auto runtime_file = arguments.configuration.value_or(std::filesystem::path{}) / "FusionCutter.ini";
    std::error_code ignored;
    if (!arguments.configuration || !std::filesystem::exists(runtime_file, ignored)) {
        report.checks.push_back({Status::NotChecked, "FusionCutter", "Configuration file",
                                 "FusionCutter.ini was not supplied or found; runtime defaults apply"});
    }

    // Rejected DLLs can lack a plugin record, so account for every explicitly requested path.
    for (const auto &requested : arguments.plugins) {
        // DLL admission stores this exact UTF-8 label of the supplied absolute path.
        const auto owner = path_text(std::filesystem::absolute(requested));
        const auto admitted = std::ranges::any_of(result.session->plugins, [&](const auto &plugin) {
            return owner == plugin->admitted.owner;
        });
        if (!admitted) {
            report.checks.push_back({Status::Failed, path_text(requested), "Plugin admission", "Requested plugin was rejected"});
        }
    }
    for (const auto &diagnostic : result.session->diagnostics) {
        report.checks.push_back(
            {Status::Failed, diagnostic.owner, "Validation diagnostic", diagnostic.message, {}, {}, diagnostic.windows_error});
    }

    for (const auto &patch : result.session->patches) {
        const auto state = patch->state.load();
        const auto id = fc::runtime::qualified_patch_id(*patch);
        fc::verify::Patch selection{id, std::string{verification_state_name(state)}, patch->reason, patch->enabled};
        if (patch->selection.variant) {
            selection.variant = patch->selection.variant_index;
            for (const auto &image : patch->selection.images) {
                selection.images.push_back({std::string{image.label}, std::string{image.profile->id}});
            }
        }
        report.patches.push_back(std::move(selection));

        // Unavailable patches retain Failed state but have no applicable plan; only startup or plan failures fail this check.
        const bool ready = state == fc::runtime::PatchState::Planning && patch->plan && !patch->plan->error();
        const bool failed = state == fc::runtime::PatchState::Failed && (patch->selected_for_startup || patch->plan);
        const auto status = ready ? Status::Passed : failed ? Status::Failed : Status::NotChecked;
        report.checks.push_back({status, id, "Patch planning and native validation",
                                 ready                   ? "Selected handler completed applicable offline validation"
                                 : patch->reason.empty() ? std::string{verification_state_name(state)}
                                                         : patch->reason});
        if (!patch->plan) {
            continue;
        }

        // Requests retained before a later plan failure remain unverified in the operation report.
        for (const auto &operation : patch->plan->operations()) {
            std::visit(
                [&](const auto &value) {
                    using T = std::remove_cvref_t<decltype(value)>;
                    std::string kind;
                    if constexpr (std::is_same_v<T, fc::runtime::ReadClaim>) {
                        kind = "Read/access";
                    } else if constexpr (std::is_same_v<T, fc::runtime::ByteWrite>) {
                        kind = "Write";
                    } else if constexpr (std::is_same_v<T, fc::runtime::OperandEdit>) {
                        kind = "Operand edit";
                    } else if constexpr (std::is_same_v<T, fc::runtime::Allocation>) {
                        kind = "Allocation";
                    } else if constexpr (std::is_same_v<T, fc::runtime::AddressFixup>) {
                        kind = "Address fixup";
                    } else if constexpr (std::is_same_v<T, fc::runtime::BranchEdit>) {
                        kind = "Branch edit";
                    } else if constexpr (std::is_same_v<T, fc::runtime::HookParticipant>) {
                        kind = "Hook";
                    } else {
                        kind = "Native callable";
                    }
                    fc::verify::Check check{ready ? Status::Passed : Status::NotChecked, id, std::move(kind),
                                            ready ? "Operation passed offline validation"
                                                  : "Plan did not complete validation: " + patch->reason};
                    if constexpr (requires { value.location; }) {
                        // Allocation access has no image location; its default location record is unused.
                        bool image_location = true;
                        if constexpr (std::is_same_v<T, fc::runtime::ReadClaim>) {
                            image_location = !(value.resource && value.resource->allocation);
                        }
                        if (image_location) {
                            check.location = fc::verify::Location{patch->plan->images().at(value.location.image).profile,
                                                                  value.location.rva, value.location.name};
                            check.evidence = evidence_text(value.evidence, *patch->plan);
                        }
                    }
                    report.checks.push_back(std::move(check));
                },
                operation);
        }
    }

    // Named assertions require a completed plan even when an unrequested patch would be a coverage gap.
    for (const auto &check : arguments.checks) {
        const auto *patch = fc::validation::find_patch(result, check);
        const bool ready =
            patch && patch->enabled && patch->state == fc::runtime::PatchState::Planning && patch->plan && !patch->plan->error();
        report.checks.push_back({ready ? Status::Passed : Status::Failed, check, "Requested patch assertion",
                                 ready                   ? "Requested patch completed applicable offline validation"
                                 : !patch                ? "Requested patch does not exist"
                                 : patch->reason.empty() ? "Requested patch is " + std::string{verification_state_name(patch->state)}
                                                         : patch->reason});
    }
    report.checks.push_back({Status::NotChecked, "Runtime Game/Multiplayer/Lobby", "Runtime service hook conflicts",
                             "Conflicts with the runtime's game and Galaxy hooks were not assessed"});
    report.checks.push_back({Status::NotChecked, "Verifier", "Live behavior",
                             "Prepare/Activate, live mappings and permissions, installation, native execution and thread timing, gameplay "
                             "and remote interoperability were not checked"});
    report.exit_code = std::ranges::any_of(report.checks,
                                           [](const auto &check) {
                                               return check.status == Status::Failed;
                                           })
                           ? 1
                           : 0;
    return report;
}

// Writes JSON when requested and preserves readable output; output failures are tool failures.
int emit_report(fc::verify::Report report, const std::optional<std::filesystem::path> &json) {
    if (json) {
        std::ofstream output(*json, std::ios::binary | std::ios::trunc);
        if (output) {
            fc::verify::write_json(output, report);
            output.close();
        }
        if (!output) {
            report.exit_code = 2;
            report.checks.push_back(
                {fc::verify::Status::Failed, path_text(*json), "JSON output", "Could not write the complete JSON report"});
        }
    }
    fc::verify::write_human(std::cout, report);
    return report.exit_code;
}
} // namespace

// Validates requested files and reports plugin and patch outcomes; invalid input or tool failures return 2.
int run(int count, wchar_t *values[]) {
    auto arguments = parse_arguments(count, values);
    if (!arguments) {
        std::cerr << "Argument error: " << arguments.error()
                  << "\n\nUsage: FusionCutterVerify --image ProfileId=Path [--image ProfileId=Path ...] "
                     "[--plugin Path ...] [--config Path] [--dedicated] [--check Plugin.Patch ...] [--json Path]\n";
        return 2;
    }

    if (auto failure = output_error(*arguments)) {
        std::cerr << "Output error: " << *failure << '\n';
        return 2;
    }

    fc::validation::ValidationInput input;
    input.role = arguments->dedicated ? FC_ROLE_SERVER : FC_ROLE_CLIENT;
    input.images = arguments->images;
    input.plugins = arguments->plugins;
    input.config_directory = arguments->configuration;
    auto validated = fc::validation::validate_offline(input);
    if (!validated) {
        fc::verify::Report report{.role = arguments->dedicated ? "Server" : "Client", .exit_code = 2};
        for (const auto &image : arguments->images) {
            report.images.push_back({path_text(image.path), image.profile});
        }
        report.checks.push_back({fc::verify::Status::Failed,
                                 "Verifier",
                                 "Input validation",
                                 validated.error().message,
                                 {},
                                 {},
                                 validated.error().windows_error});
        return emit_report(std::move(report), arguments->json);
    }
    return emit_report(make_report(*arguments, *validated), arguments->json);
}

// Runs the verifier and converts unexpected exceptions into tool errors with exit code 2.
int wmain(int count, wchar_t *values[]) {
    // Return DLL loading errors to the verifier so Windows dialogs cannot block command-line use.
    SetErrorMode(GetErrorMode() | SEM_FAILCRITICALERRORS);

    try {
        return run(count, values);
    } catch (const std::exception &error) {
        std::cerr << "Verifier failed: " << error.what() << '\n';
        return 2;
    } catch (...) {
        std::cerr << "Verifier failed with an unknown exception.\n";
        return 2;
    }
}
