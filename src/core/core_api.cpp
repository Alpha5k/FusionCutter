#include <FusionCutter/LoaderApi.h>

#include "catalog/generated_catalog.hpp"
#include "reporting/crash_reporter.hpp"
#include "reporting/session.hpp"
#include "startup.hpp"
#include "targets/process_images.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace {

static_assert(sizeof(FC_HostRole) == sizeof(std::uint32_t));
static_assert(sizeof(FC_QueryResult) == sizeof(std::uint32_t));
static_assert(sizeof(FC_InitializeResult) == sizeof(std::uint32_t));
static_assert(std::is_standard_layout_v<FC_LoaderStartupInfo>);
static_assert(std::is_standard_layout_v<FC_InitializeArgs>);
static_assert(std::is_standard_layout_v<FC_CoreApi>);
static_assert(offsetof(FC_InitializeArgs, host_role) == sizeof(std::uint32_t));

constexpr std::uint32_t kMinimumInitializeArgsSize = offsetof(FC_InitializeArgs, loader_startup);

struct ProcessState {
    std::mutex initialization_mutex;
    std::optional<fusioncutter::HostRole> role;
    FC_InitializeResult result{FC_INIT_FATAL};
    std::unique_ptr<fusioncutter::StartupState> startup;
    std::atomic<fusioncutter::StartupState*> published_startup{};
    std::atomic_uint64_t published_status_revision{};
    std::vector<fusioncutter::PatchOutcome> reported_patch_outcomes;
    bool reported_runtime_fatal{};
};

ProcessState g_process_state;

[[nodiscard]] fusioncutter::HostRole host_role(FC_HostRole role) noexcept {
    return role == FC_HOST_ROLE_CLIENT ? fusioncutter::HostRole::Client : fusioncutter::HostRole::Server;
}

[[nodiscard]] constexpr fusioncutter::catalog::CatalogScope catalog_scope() noexcept {
#if defined(_M_IX86)
    constexpr auto architecture = fusioncutter::Architecture::X86;
#else
    constexpr auto architecture = fusioncutter::Architecture::X64;
#endif
    return {
        architecture,
        (FC_CORE_SUPPORTED_ROLE_MASK & FC_HOST_ROLE_CLIENT) != 0,
        (FC_CORE_SUPPORTED_ROLE_MASK & FC_HOST_ROLE_SERVER) != 0,
    };
}

[[nodiscard]] std::string_view selected_proxy(const FC_InitializeArgs& args) noexcept {
    constexpr auto kStartupEnd = offsetof(FC_InitializeArgs, loader_startup) + sizeof(FC_LoaderStartupInfo);
    if (args.struct_size < kStartupEnd || args.loader_startup.struct_size < sizeof(FC_LoaderStartupInfo)) {
        return {};
    }

    const auto& name = args.loader_startup.selected_proxy_basename;
    const auto end = std::ranges::find(name, '\0');
    return {name, static_cast<std::size_t>(end - std::begin(name))};
}

[[nodiscard]] FC_InitializeResult loader_result(fusioncutter::InitializationOutcome outcome) noexcept {
    switch (outcome) {
    case fusioncutter::InitializationOutcome::Completed:
        return FC_INIT_COMPLETED;
    case fusioncutter::InitializationOutcome::Unsupported:
        return FC_INIT_UNSUPPORTED;
    case fusioncutter::InitializationOutcome::Fatal:
        return FC_INIT_FATAL;
    }
    return FC_INIT_FATAL;
}

[[nodiscard]] fusioncutter::OutcomeReason core_reason(std::string message, std::string operation) {
    return {std::move(message), std::move(operation), {}};
}

[[nodiscard]] std::vector<fusioncutter::reporting::StatusContributorRef>
reporting_contributors(const fusioncutter::StartupState& startup) {
    std::vector<fusioncutter::reporting::StatusContributorRef> contributors;
    contributors.reserve(startup.status_contributors().size());
    for (const auto& contributor : startup.status_contributors()) {
        contributors.push_back({contributor.name, contributor.contributor});
    }
    return contributors;
}

void report_patch_result(const fusioncutter::PatchResult& patch) noexcept {
    if (patch.outcome == fusioncutter::PatchOutcome::Installed) {
        fusioncutter::reporting::publish_installed_patch(patch.patch_id.data());
        fusioncutter::logging::info(patch.patch_id, "Patch installed");
        return;
    }
    if ((patch.outcome != fusioncutter::PatchOutcome::Failed && patch.outcome != fusioncutter::PatchOutcome::Skipped) ||
        !patch.reason.has_value()) {
        return;
    }

    const auto& reason = *patch.reason;
    const auto operation = reason.operation.value_or(std::string{});
    const auto related = reason.related_patch.value_or(fusioncutter::PatchId{});
    if (patch.outcome == fusioncutter::PatchOutcome::Failed) {
        fusioncutter::logging::error(patch.patch_id, reason.message, operation, related);
    } else {
        fusioncutter::logging::warning(patch.patch_id, reason.message, operation, related);
    }
}

[[nodiscard]] FC_InitializeResult
finish_initialization(fusioncutter::HostRole role, const fusioncutter::InitializationResult& initialization,
                      std::span<const fusioncutter::PatchResult> patch_results = {},
                      std::span<const fusioncutter::reporting::StatusContributorRef> contributors = {}) {
    if (initialization.reason.has_value()) {
        const auto& reason = *initialization.reason;
        const auto operation = reason.operation.value_or(std::string{});
        const auto related_patch = reason.related_patch.value_or(fusioncutter::PatchId{});
        if (initialization.outcome == fusioncutter::InitializationOutcome::Fatal) {
            fusioncutter::logging::error("Core", reason.message, operation, related_patch);
        } else if (initialization.outcome == fusioncutter::InitializationOutcome::Unsupported) {
            fusioncutter::logging::warning("Core", reason.message, operation, related_patch);
        }
    }

    auto& reporting = fusioncutter::reporting::Session::instance();
    reporting.publish_status(initialization, patch_results, contributors);
    if (role == fusioncutter::HostRole::Server &&
        initialization.outcome == fusioncutter::InitializationOutcome::Fatal) {
        reporting.flush();
    }
    return loader_result(initialization.outcome);
}

[[nodiscard]] std::expected<std::optional<fusioncutter::TargetContext>, fusioncutter::OutcomeReason>
probe_late_image(fusioncutter::TargetLayout layout, fusioncutter::HostRole role, fusioncutter::TargetImage image) {
    auto recognized = fusioncutter::targets::recognize_loaded_process_image(layout, role, image);
    if (!recognized.has_value()) {
        return std::unexpected(fusioncutter::OutcomeReason{
            std::string(recognized.error().detail) + " (Windows error " +
                std::to_string(recognized.error().windows_error) + ")",
            "Recognize late image",
            {},
        });
    }
    if (!recognized->has_value()) {
        return std::optional<fusioncutter::TargetContext>{};
    }
    return std::optional<fusioncutter::TargetContext>{(**recognized).context};
}

[[nodiscard]] FC_InitializeResult initialize_once(const FC_InitializeArgs& args, fusioncutter::HostRole role) {
    const auto crash_reporter = fusioncutter::reporting::install_crash_reporter(role, selected_proxy(args));
    if (!crash_reporter.has_value()) {
        auto& reporting = fusioncutter::reporting::Session::instance();
        reporting.start(role);
        return finish_initialization(role,
                                     {fusioncutter::InitializationOutcome::Fatal,
                                      core_reason(std::string(crash_reporter.error().detail) + " (Windows error " +
                                                      std::to_string(crash_reporter.error().windows_error) + ")",
                                                  "Install crash reporter")});
    }

    auto& reporting = fusioncutter::reporting::Session::instance();
    reporting.start(role);

    fusioncutter::reporting::publish_crash_phase(fusioncutter::reporting::CorePhase::TargetRecognition);
    auto images = fusioncutter::targets::recognize_current_process_images(role);
    if (!images.has_value()) {
        fusioncutter::reporting::publish_crash_phase(fusioncutter::reporting::CorePhase::Failed);
        return finish_initialization(role, {fusioncutter::InitializationOutcome::Unsupported,
                                            core_reason(std::string(images.error().detail) + " (Windows error " +
                                                            std::to_string(images.error().windows_error) + ")",
                                                        "Recognize target")});
    }

    const auto& primary_image = images->game_module.has_value() ? *images->game_module : images->executable;
    fusioncutter::reporting::publish_crash_target(primary_image.context, primary_image.fingerprint);
    reporting.set_target(primary_image.context);

    auto catalog =
        fusioncutter::catalog::initialize_catalog(fusioncutter::catalog::generated_catalog_entries(), catalog_scope());
    if (!catalog.has_value()) {
        fusioncutter::reporting::publish_crash_phase(fusioncutter::reporting::CorePhase::Failed);
        return finish_initialization(role, {fusioncutter::InitializationOutcome::Fatal, catalog.error()});
    }

    fusioncutter::reporting::publish_crash_phase(fusioncutter::reporting::CorePhase::Configuration);
    auto path = fusioncutter::config::configuration_path(role);
    if (!path.has_value()) {
        fusioncutter::reporting::publish_crash_phase(fusioncutter::reporting::CorePhase::Failed);
        return finish_initialization(role, {fusioncutter::InitializationOutcome::Fatal, path.error()});
    }
    reporting.set_configuration(*path);
    const auto applicable = fusioncutter::catalog::configurable_patches(*catalog, primary_image.context);
    auto configuration = fusioncutter::config::load_configuration(*path, applicable);
    if (!configuration.has_value()) {
        fusioncutter::reporting::publish_crash_phase(fusioncutter::reporting::CorePhase::Failed);
        return finish_initialization(role, {fusioncutter::InitializationOutcome::Fatal, configuration.error()});
    }

    reporting.set_level(configuration->log_level());
    for (const auto& diagnostic : configuration->diagnostics()) {
        fusioncutter::logging::warning(
            "Core", "Configuration line " + std::to_string(diagnostic.line) + ": " + diagnostic.message,
            "Read configuration");
    }
    if (configuration->omitted_diagnostics() != 0) {
        fusioncutter::logging::warning("Core",
                                       std::to_string(configuration->omitted_diagnostics()) +
                                           " additional configuration diagnostics omitted",
                                       "Read configuration");
    }
    if (configuration->output_error().has_value()) {
        const auto& output_error = *configuration->output_error();
        fusioncutter::logging::warning("Core", output_error.message, output_error.operation.value_or(std::string{}));
    }

    std::vector<fusioncutter::TargetContext> startup_images{images->executable.context};
    if (images->game_module.has_value()) {
        startup_images.push_back(images->game_module->context);
    }

    fusioncutter::reporting::publish_crash_phase(fusioncutter::reporting::CorePhase::PatchPlanning);
    auto startup = fusioncutter::run_startup(std::move(*catalog), std::move(*configuration), startup_images);
    for (const auto& patch : startup.patch_results()) {
        report_patch_result(patch);
    }

    g_process_state.startup = std::make_unique<fusioncutter::StartupState>(std::move(startup));
    g_process_state.reported_patch_outcomes.clear();
    g_process_state.reported_patch_outcomes.reserve(g_process_state.startup->patch_results().size());
    for (const auto& patch : g_process_state.startup->patch_results()) {
        g_process_state.reported_patch_outcomes.push_back(patch.outcome);
    }
    g_process_state.published_startup.store(g_process_state.startup.get(), std::memory_order_release);
    g_process_state.published_status_revision.store(g_process_state.startup->status_revision(),
                                                    std::memory_order_release);
    const auto contributors = reporting_contributors(*g_process_state.startup);
    const auto result = finish_initialization(role, g_process_state.startup->initialization_result(),
                                              g_process_state.startup->patch_results(), contributors);
    fusioncutter::reporting::publish_crash_phase(result == FC_INIT_FATAL
                                                     ? fusioncutter::reporting::CorePhase::Failed
                                                     : fusioncutter::reporting::CorePhase::Completed);
    return result;
}

FC_InitializeResult FC_CALL initialize(const FC_InitializeArgs* args) {
    if (args == nullptr || args->struct_size < kMinimumInitializeArgsSize) {
        return FC_INIT_FATAL;
    }

    try {
        if ((args->host_role != FC_HOST_ROLE_CLIENT && args->host_role != FC_HOST_ROLE_SERVER) ||
            (FC_CORE_SUPPORTED_ROLE_MASK & args->host_role) == 0) {
            return FC_INIT_UNSUPPORTED;
        }

        const auto role = host_role(args->host_role);
        const std::scoped_lock lock(g_process_state.initialization_mutex);
        if (g_process_state.role.has_value()) {
            return *g_process_state.role == role ? g_process_state.result : FC_INIT_UNSUPPORTED;
        }

        g_process_state.role = role;
        g_process_state.result = initialize_once(*args, role);
        return g_process_state.result;
    } catch (...) {
        fusioncutter::reporting::publish_crash_phase(fusioncutter::reporting::CorePhase::Failed);
        if (g_process_state.role.has_value()) {
            g_process_state.result = FC_INIT_FATAL;
            const fusioncutter::InitializationResult failure{
                fusioncutter::InitializationOutcome::Fatal,
                core_reason("core initialization ended unexpectedly", "Initialize core"),
            };
            static_cast<void>(finish_initialization(*g_process_state.role, failure));
        }
        return FC_INIT_FATAL;
    }
}

void FC_CALL update() {
    try {
        if (auto* startup = g_process_state.published_startup.load(std::memory_order_acquire); startup != nullptr) {
            fusioncutter::reporting::publish_crash_phase(fusioncutter::reporting::CorePhase::Runtime);
            startup->update(probe_late_image);
            const auto revision = startup->status_revision();
            if (g_process_state.published_status_revision.exchange(revision, std::memory_order_acq_rel) != revision) {
                const auto results = startup->patch_results();
                if (results.size() == g_process_state.reported_patch_outcomes.size()) {
                    for (std::size_t index = 0; index < results.size(); ++index) {
                        if (g_process_state.reported_patch_outcomes[index] != results[index].outcome) {
                            g_process_state.reported_patch_outcomes[index] = results[index].outcome;
                            report_patch_result(results[index]);
                        }
                    }
                }
                const auto& initialization = startup->initialization_result();
                if (!g_process_state.reported_runtime_fatal &&
                    initialization.outcome == fusioncutter::InitializationOutcome::Fatal &&
                    initialization.reason.has_value()) {
                    g_process_state.reported_runtime_fatal = true;
                    const auto& reason = *initialization.reason;
                    fusioncutter::logging::error("Core", reason.message, reason.operation.value_or(std::string{}),
                                                 reason.related_patch.value_or(fusioncutter::PatchId{}));
                }
                const auto contributors = reporting_contributors(*startup);
                fusioncutter::reporting::Session::instance().publish_status(initialization, results, contributors);
            }
        }
    } catch (...) {
        // The host-pumped ABI operation must never allow a C++ exception to cross the DLL boundary.
    }
}

void FC_CALL report_host_event(FC_HostEventLevel level, const char* message) {
    try {
        if (message == nullptr) {
            return;
        }
        switch (level) {
        case FC_HOST_EVENT_ERROR:
            fusioncutter::logging::error("Core", message, "Loader event");
            break;
        case FC_HOST_EVENT_WARNING:
            fusioncutter::logging::warning("Core", message, "Loader event");
            break;
        case FC_HOST_EVENT_INFO:
            fusioncutter::logging::info("Core", message, "Loader event");
            break;
        case FC_HOST_EVENT_DEBUG:
            fusioncutter::logging::debug("Core", message, "Loader event");
            break;
        default:
            break;
        }
    } catch (...) {
        // The reporting bridge is best-effort and must never throw into a loader.
    }
}

constexpr FC_CoreApi kCoreApi = {
    sizeof(FC_CoreApi), FC_ABI_GENERATION, FC_CORE_SUPPORTED_ROLE_MASK, initialize, update, report_host_event,
};

FC_QueryResult query_core_api(uint32_t requested_generation, uint32_t caller_table_capacity, FC_CoreApi* output_table) {
    if (output_table == nullptr) {
        return FC_QUERY_INVALID_ARGUMENT;
    }

    if (requested_generation != FC_ABI_GENERATION) {
        return FC_QUERY_UNSUPPORTED_GENERATION;
    }

    const auto copy_size = std::min<std::size_t>(caller_table_capacity, sizeof(kCoreApi));
    if (copy_size != 0) {
        std::memcpy(output_table, &kCoreApi, copy_size);
    }

    if (caller_table_capacity < sizeof(kCoreApi)) {
        return FC_QUERY_TABLE_TOO_SMALL;
    }

    return FC_QUERY_OK;
}

} // namespace

extern "C" FC_QueryResult FC_CALL FusionCutter_GetCoreApi(uint32_t requested_generation, uint32_t caller_table_capacity,
                                                          FC_CoreApi* output_table) {
    try {
        return query_core_api(requested_generation, caller_table_capacity, output_table);
    } catch (...) {
        return FC_QUERY_INVALID_ARGUMENT;
    }
}
