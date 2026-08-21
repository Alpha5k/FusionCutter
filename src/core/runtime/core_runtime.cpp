#include "core_runtime.hpp"

#include "../fatal_boundary.hpp"
#include "../catalog/catalog_builder.hpp"
#include "../catalog/definition_copy.hpp"
#include "../planning/plan_validation.hpp"
#include "../planning/resolution.hpp"
#include "installation.hpp"
#include "late_images.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <exception>
#include <filesystem>
#include <limits>
#include <string_view>
#include <utility>

namespace fc::runtime {
namespace {

// Startup failures use the compact patch FailureReason so later reporting receives one actionable cause.
[[nodiscard]] planning::FailureReason startup_failure(std::string message, std::string operation) {
    return {.message = std::move(message), .phase = planning::PatchPhase::Selection, .operation = std::move(operation)};
}

// The framework derives adjacent configuration and plugin paths from its retained module, never the process CWD.
[[nodiscard]] std::expected<std::filesystem::path, std::string> installation_directory() {
    HMODULE module{};
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&installation_directory), &module) == 0 ||
        module == nullptr) {
        return std::unexpected("FusionCutter.dll module identity is unavailable");
    }
    std::array<wchar_t, 32'768> path{};
    const auto length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) {
        return std::unexpected("FusionCutter.dll path is unavailable");
    }
    return std::filesystem::path{std::wstring_view{path.data(), length}}.parent_path();
}

// These focused predicates validate the complete loader identity before target recognition can observe the process.
[[nodiscard]] bool known_chain(FC_DirectInputChainResult result) noexcept {
    return result >= FC_DIRECT_INPUT_CHAIN_NOT_APPLICABLE && result <= FC_DIRECT_INPUT_CHAIN_AMBIGUOUS;
}

[[nodiscard]] bool legal_tuple(FC_HostRole role, FC_LoaderKind loader, FC_DirectInputChainResult chain) noexcept {
    if (loader == FC_LOADER_KIND_DINPUT8) {
        return role == FC_HOST_ROLE_CLIENT && chain != FC_DIRECT_INPUT_CHAIN_NOT_APPLICABLE && known_chain(chain);
    }
    if (loader == FC_LOADER_KIND_BATTLEFRONT2) {
        return role == FC_HOST_ROLE_CLIENT && chain == FC_DIRECT_INPUT_CHAIN_NOT_APPLICABLE;
    }
    if (loader == FC_LOADER_KIND_RCONSERVER) {
        return role == FC_HOST_ROLE_SERVER && chain == FC_DIRECT_INPUT_CHAIN_NOT_APPLICABLE;
    }
    return false;
}

// These display helpers keep framework diagnostics readable without exposing status rendering as a second API.
[[nodiscard]] std::string_view role_name(FC_HostRole role) noexcept {
    return role == FC_HOST_ROLE_SERVER ? "Server" : "Client";
}

[[nodiscard]] std::string_view loader_name(LoaderKind loader) noexcept {
    switch (loader) {
    case LoaderKind::DirectInput8:
        return "DInput8";
    case LoaderKind::RconServer:
        return "RconServer";
    case LoaderKind::Battlefront2:
        return "Battlefront2";
    default:
        return "Unknown";
    }
}

[[nodiscard]] std::string_view layout_name(FC_TargetLayout layout) noexcept {
    switch (layout) {
    case FC_LAYOUT_GAMESPY_RETAIL:
        return "GameSpy Retail";
    case FC_LAYOUT_STEAM_RETAIL:
        return "Steam Retail";
    case FC_LAYOUT_GOG_RETAIL:
        return "GOG Retail";
    case FC_LAYOUT_MOD_TOOLS:
        return "Mod Tools";
    case FC_LAYOUT_CLASSIC_COLLECTION:
        return "Classic Collection";
    default:
        return "Unknown";
    }
}

[[nodiscard]] std::string_view admission_stage_name(catalog::AdmissionStage stage) noexcept {
    switch (stage) {
    case catalog::AdmissionStage::Discovery:
        return "Discovery";
    case catalog::AdmissionStage::Architecture:
        return "Architecture";
    case catalog::AdmissionStage::Loading:
        return "Loading";
    case catalog::AdmissionStage::Query:
        return "Query";
    case catalog::AdmissionStage::Registration:
        return "Registration";
    case catalog::AdmissionStage::Copying:
        return "Copying";
    case catalog::AdmissionStage::Structure:
        return "Structure";
    case catalog::AdmissionStage::Collision:
        return "Collision";
    case catalog::AdmissionStage::Capacity:
        return "Capacity";
    case catalog::AdmissionStage::Configuration:
        return "Configuration";
    }
    return "Unknown";
}

// Logging of plugin admission runs after framework configuration so early decisions honor the final log filter.
void log_admission(const catalog::CatalogBuildResult& built, CoreLogger logger) noexcept {
    try {
        if (built.catalog) {
            logger.info("Admitted {} plugin(s), {} patch(es), and {} group(s); rejected {} contribution(s)",
                        built.catalog->plugins().size(), built.catalog->patch_count(), built.catalog->group_count(),
                        built.rejections.size());
        }
        for (const auto& rejection : built.rejections) {
            const auto identity = rejection.plugin_id
                                      ? *rejection.plugin_id
                                      : (rejection.path ? rejection.path->filename().string() : "Unknown plugin");
            logger.warning("Rejected '{}' during {}: {}", identity, admission_stage_name(rejection.stage),
                           rejection.reason);
        }
    } catch (...) {
        logger.error("Plugin admission results could not be rendered as diagnostics");
    }
}

// Loaded configuration retains recoverable diagnostics as data; this function publishes them to the ordinary log.
void log_configuration(const config::ConfigurationSnapshot& configuration, CoreLogger logger) noexcept {
    try {
        logger.info("Loaded configuration for {} admitted plugin(s)", configuration.plugins.size());
        for (const auto& plugin : configuration.plugins) {
            if (plugin.file_created) {
                logger.debug("Created the default configuration for plugin '{}'", plugin.plugin_id);
            }
            if (plugin.output_error) {
                logger.warning("Configuration output for plugin '{}' is degraded: {}", plugin.plugin_id,
                               *plugin.output_error);
            }
            for (const auto& diagnostic : plugin.diagnostics) {
                if (diagnostic.line == 0) {
                    logger.warning("Configuration for plugin '{}': {}", plugin.plugin_id, diagnostic.message);
                } else {
                    logger.warning("Configuration for plugin '{}' at line {}: {}", plugin.plugin_id, diagnostic.line,
                                   diagnostic.message);
                }
            }
        }
    } catch (...) {
        logger.error("Configuration results could not be rendered as diagnostics");
    }
}

} // namespace

CoreRuntime::CoreRuntime() noexcept
    : crash_(crash_phases_), host_api_{.struct_size = sizeof(FC_HostApi),
                                       .context = this,
                                       .log_enabled = &log_enabled,
                                       .log_write = &log_write,
                                       .trace_enabled = &trace_enabled,
                                       .trace_try_write = &trace_try_write,
                                       .trace_health = &trace_health} {
    // All invariant failures after the Commit phase converge here before any installed callback can execute.
    install_fatal_handler(this, &fatal_handler);
}

InitializationResult CoreRuntime::finish(InitializationResult result, std::optional<planning::FailureReason> reason) {
    // This record is authoritative for later Initialize calls, including fatal or unsupported first results.
    initialization_.emplace(result, std::move(reason));
    // No transient patch cursor may survive the terminal startup boundary seen by status or crash capture.
    const auto phase = result == InitializationResult::Completed ? CorePhase::Running : CorePhase::Idle;
    crash_.set_core_phase(phase);
    crash_.clear_current_patch();
    const auto status = result == InitializationResult::Completed ? reporting::InitializationStatus::Completed
                                                                  : (result == InitializationResult::Unsupported
                                                                         ? reporting::InitializationStatus::Unsupported
                                                                         : reporting::InitializationStatus::Fatal);
    auto logger = reporting_.logger("Runtime");
    if (result == InitializationResult::Completed) {
        logger.info("Fusion Cutter initialization completed");
    } else if (result == InitializationResult::Unsupported) {
        logger.warning("Fusion Cutter initialization stopped because the host is unsupported: {}",
                       initialization_->reason ? initialization_->reason->message : "No recognition result");
    } else {
        logger.error("Fusion Cutter initialization failed in '{}': {}",
                     initialization_->reason && initialization_->reason->operation ? *initialization_->reason->operation
                                                                                   : "Initialize Fusion Cutter",
                     initialization_->reason ? initialization_->reason->message : "No failure reason was retained");
    }
    // Every Initialize return owns one status opportunity; a Fatal result also flushes accepted startup logs.
    reporting_.publish(status, initialization_->reason ? &*initialization_->reason : nullptr,
                       patch_runtime_ ? &*patch_runtime_ : nullptr, traces_, true);
    return result;
}

InitializationResult CoreRuntime::initialize(const InitializationRequest& request) {
    if (initialization_) {
        return initialization_->result;
    }
    // Retain the copied loader facts for later reporting; no caller-owned C view survives this assignment.
    request_ = request;
    reporting_.start({.started = std::chrono::system_clock::now(),
                      .role = request.role,
                      .loader_kind = static_cast<std::uint32_t>(request.loader.kind),
                      .direct_input_chain = static_cast<std::uint32_t>(request.loader.direct_input_chain),
                      .selected_proxy_basename = request.loader.selected_proxy_basename});
    crash_.set_session(request.role, request.loader.selected_proxy_basename);
    // Crash-safe fixed storage precedes every operation that may execute provisional or admitted native code.
    if (!crash_.install()) {
        reporting_.logger("CrashCapture").error("Crash snapshot storage could not be prepared");
        return finish(InitializationResult::Fatal,
                      startup_failure("Crash snapshot storage could not be prepared", "Install crash capture"));
    }
    crash_.set_core_phase(CorePhase::Startup);

    // Target recognition is the first operation allowed to distinguish an unsupported host from a fatal startup.
    crash_.set_core_phase(CorePhase::TargetRecognition);
    auto target = targets::recognize_target(request.role);
    if (!target) {
        const auto result = target.error().kind == targets::RecognitionErrorKind::Unsupported
                                ? InitializationResult::Unsupported
                                : InitializationResult::Fatal;
        auto logger = reporting_.logger("TargetRecognition");
        if (result == InitializationResult::Unsupported) {
            logger.warning("Target recognition stopped: {}", target.error().message);
        } else {
            logger.error("Target recognition failed in '{}': {}", target.error().operation, target.error().message);
        }
        return finish(result, startup_failure(std::move(target.error().message), std::move(target.error().operation)));
    }
    crash_.set_target(*target);

    auto directory = installation_directory();
    if (!directory) {
        return finish(InitializationResult::Fatal,
                      startup_failure(std::move(directory.error()), "Resolve installation directory"));
    }

    // Plugin admission validates all plugins and configuration before transferring ownership.
    crash_.set_core_phase(CorePhase::PluginAdmission);
    const catalog::CatalogBuildObserver build_observer{
        .context = &crash_, .begin_configuration = [](void* context) noexcept {
            static_cast<CrashReporter*>(context)->set_core_phase(CorePhase::Configuration);
        }};
    auto built = catalog::acquire_catalog(host_api_, *target, *directory, build_observer);
    if (built.fatal_error || !built.catalog || !built.configuration) {
        auto message = built.fatal_error.value_or("Plugin admission did not produce final state");
        reporting_.logger("PluginAdmission").error("Plugin catalog construction failed: {}", message);
        return finish(InitializationResult::Fatal, startup_failure(std::move(message), "Build plugin catalog"));
    }
    // The builder's observed transition attributes its internal configuration callbacks at their actual boundary.
    reporting_.configure(built.configuration->framework.log_level);
    const auto tracing_logger = reporting_.logger("Tracing");
    traces_.configure(built.configuration->framework.max_trace_size_mb, *directory, tracing_logger);
    // Republish early startup outcomes only after framework configuration establishes the final log filter.
    reporting_.logger("Runtime").debug("Accepted {} startup through the {} loader", role_name(request.role),
                                       loader_name(request.loader.kind));
    reporting_.logger("CrashCapture").debug("Crash capture is active");
    reporting_.logger("TargetRecognition")
        .info("Recognized {} {} target ({})", layout_name(target->layout()), role_name(target->role()),
              target->architecture() == FC_ARCH_X64 ? "x64" : "x86");
    log_admission(built, reporting_.logger("PluginAdmission"));
    log_configuration(*built.configuration, reporting_.logger("Configuration"));
    if (built.configuration->framework.max_trace_size_mb == 0) {
        tracing_logger.info("High-volume tracing is disabled by configuration");
    } else {
        tracing_logger.debug("High-volume tracing is configured with a {} MiB file limit",
                             built.configuration->framework.max_trace_size_mb);
    }

    // The runtime owner is emplaced once and never moved because all later views and callback records borrow from it.
    patch_runtime_.emplace(std::move(*target), std::move(*built.catalog));
    patch_runtime_->interfaces.set_logger(reporting_.logger("Interfaces"));
    traces_.set_catalog(patch_runtime_->catalog);
    reporting_.set_target(patch_runtime_->target);
    reporting_.set_catalog(patch_runtime_->catalog, built.rejections);
    crash_.publish_catalog(patch_runtime_->catalog);

    crash_.set_core_phase(CorePhase::Validation);
    patch_runtime_->patches = planning::resolve_patches({.catalog = patch_runtime_->catalog,
                                                         .target = patch_runtime_->target,
                                                         .configuration = *built.configuration,
                                                         .logger = reporting_.logger("Selection")});
    // The one bounded mask is derived only after selection has fixed every waiting record's support image.
    patch_runtime_->awaited_images.reset_from_waiting(patch_runtime_->catalog, patch_runtime_->patches);
    auto plan = planning::build_installation_plan(patch_runtime_->target, patch_runtime_->patches, {},
                                                  reporting_.logger("Validation"));

    crash_.set_core_phase(CorePhase::Installation);
    const auto installed =
        install_ready_patches(plan, *patch_runtime_, traces_, crash_, {.logger = reporting_.logger("Installation")});
    if (installed == InstallationResult::Fatal) {
        return finish(InitializationResult::Fatal,
                      startup_failure("A required patch prevented safe startup", "Install ready patches"));
    }
    return finish(InitializationResult::Completed);
}

InitializationResult CoreRuntime::reject_initialization(std::string reason) noexcept {
    if (initialization_) {
        return initialization_->result;
    }
    try {
        crash_.set_session(FC_HOST_ROLE_CLIENT, {});
        static_cast<void>(crash_.install());
        reporting_.start({.started = std::chrono::system_clock::now(), .role = FC_HOST_ROLE_CLIENT});
        return finish(InitializationResult::Fatal,
                      startup_failure(std::move(reason), "Validate loader initialization"));
    } catch (...) {
        return InitializationResult::Fatal;
    }
}

std::optional<InitializationResult> CoreRuntime::initialization_result() const noexcept {
    return initialization_ ? std::optional{initialization_->result} : std::nullopt;
}

void CoreRuntime::update() noexcept {
    // Unsupported, fatal, and uninitialized framework instances cannot acquire another result through Update callbacks.
    if (!initialization_ || initialization_->result != InitializationResult::Completed || !patch_runtime_) {
        return;
    }
    try {
        // A newly installed late patch joins update_order inside the common installer and runs later in this same call.
        const LateImageServices services{.installation = {.logger = reporting_.logger("Installation")},
                                         .logger = reporting_.logger("LateImages"),
                                         .planning_logger = reporting_.logger("Validation")};
        if (process_awaited_images(*patch_runtime_, traces_, crash_, services) == LateImageResult::Fatal) {
            fatal_invariant("Installation for a late image violated a required runtime invariant");
        }
        crash_.set_core_phase(CorePhase::Running);
        // InstalledRecord indexes preserve case-insensitive patch ID order without duplicating callback ownership.
        for (const auto index : patch_runtime_->update_order) {
            auto& installed = patch_runtime_->installed_patches[index];
            crash_.set_current_patch(installed.patch, planning::PatchPhase::Activate);
            installed.instance.update(catalog::report_token(installed.patch));
            crash_.clear_current_patch();
        }
        // Live contributors run after the Update callback on this serialized pump; unchanged text is not rewritten.
        reporting_.publish(reporting::InitializationStatus::Completed, nullptr, &*patch_runtime_, traces_, false);
    } catch (...) {
        crash_.clear_current_patch();
        fatal_invariant("An exception escaped the serialized framework Update callback boundary");
    }
}

const FC_HostApi& CoreRuntime::host_api() const noexcept {
    return host_api_;
}

const CrashReporter& CoreRuntime::crash_reporter() const noexcept {
    return crash_;
}

const TraceSession& CoreRuntime::traces() const noexcept {
    return traces_;
}

FC_Bool FC_CALL CoreRuntime::log_enabled(void* context, FC_ReportToken report, FC_LogLevel level) noexcept {
    return context == nullptr ? FC_FALSE : static_cast<CoreRuntime*>(context)->reporting_.enabled(report, level);
}

void FC_CALL CoreRuntime::log_write(void* context, FC_ReportToken report, FC_LogLevel level,
                                    FC_StringView message) noexcept {
    if (context != nullptr) {
        static_cast<CoreRuntime*>(context)->reporting_.write(report, level, message);
    }
}

FC_Bool FC_CALL CoreRuntime::trace_enabled(void* context, FC_TraceHandle trace) noexcept {
    return context == nullptr ? FC_FALSE : static_cast<CoreRuntime*>(context)->traces_.enabled(trace);
}

FC_Bool FC_CALL CoreRuntime::trace_try_write(void* context, FC_TraceHandle trace, FC_ByteView record) noexcept {
    return context == nullptr ? FC_FALSE : static_cast<CoreRuntime*>(context)->traces_.try_write(trace, record);
}

void FC_CALL CoreRuntime::trace_health(void* context, FC_TraceHandle trace, FC_TraceHealth* output) noexcept {
    if (context != nullptr) {
        static_cast<CoreRuntime*>(context)->traces_.health(trace, output);
    }
}

void CoreRuntime::fatal_handler(void* context, std::string_view reason) noexcept {
    auto& runtime = *static_cast<CoreRuntime*>(context);
    runtime.crash_.clear_current_patch();
    runtime.crash_.set_core_phase(CorePhase::Idle);
    runtime.reporting_.fatal(reason, runtime.patch_runtime_ ? &*runtime.patch_runtime_ : nullptr, runtime.traces_);
}

std::expected<InitializationRequest, std::string> copy_initialization_request(const FC_InitializeArgs* arguments) {
    // The independently sized outer record may have a tail, but generation 1 requires its complete known prefix.
    constexpr auto required_size = offsetof(FC_InitializeArgs, loader_startup) + sizeof(arguments->loader_startup);
    if (arguments == nullptr || arguments->struct_size < required_size) {
        return std::unexpected("The loader initialization record is null or undersized");
    }
    // Unknown scalar values are rejected before the tuple can accidentally participate in target selection.
    const auto& startup = arguments->loader_startup;
    if ((arguments->host_role != FC_HOST_ROLE_CLIENT && arguments->host_role != FC_HOST_ROLE_SERVER) ||
        startup.loader_kind < FC_LOADER_KIND_DINPUT8 || startup.loader_kind > FC_LOADER_KIND_BATTLEFRONT2 ||
        !known_chain(startup.direct_input_chain)) {
        return std::unexpected("The loader initialization record contains an unknown role or enumeration value");
    }
    // The fixed display field must terminate in bounds and contain valid UTF-8 before it becomes an owned string.
    const auto* terminator = static_cast<const char*>(
        std::memchr(startup.selected_proxy_basename, '\0', FC_SELECTED_PROXY_BASENAME_CAPACITY));
    if (terminator == nullptr) {
        return std::unexpected("The selected proxy basename is not NUL-terminated");
    }
    const std::string_view basename{startup.selected_proxy_basename,
                                    static_cast<std::size_t>(terminator - startup.selected_proxy_basename)};
    if (!catalog::valid_utf8(basename)) {
        return std::unexpected("The selected proxy basename is not valid UTF-8");
    }
    // Known values are still fatal when their role, loader, and chain combination is not one supported entry route.
    if (!legal_tuple(arguments->host_role, startup.loader_kind, startup.direct_input_chain)) {
        return std::unexpected("The loader kind, role, and DirectInput chain result form an illegal tuple");
    }

    // The returned value is the ownership boundary: every later subsystem sees only framework-owned representations.
    return InitializationRequest{
        .role = arguments->host_role,
        .loader = {.kind = static_cast<LoaderKind>(startup.loader_kind),
                   .direct_input_chain = static_cast<DirectInputChainResult>(startup.direct_input_chain),
                   .windows_error = startup.windows_error,
                   .wildcard_match_count = startup.wildcard_match_count,
                   .selected_proxy_basename = std::string{basename}}};
}

} // namespace fc::runtime
