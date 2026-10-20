#pragma once

#include "configuration/configuration.hpp"
#include "plugins/plugin.hpp"

#include <memory>
#include <chrono>
#include <atomic>
#include <cstdint>
#include <string_view>

namespace fc::runtime {

class SubmittedPlan;
class PatchApiReservations;
class CoreRuntime;

// Records a patch's selection and installation stage; Installed includes completed activation.
enum class PatchState {
    Disabled,
    // Awaiting a required image or provider.
    Waiting,
    // Ready for handler construction and plan collection.
    Planning,
    Preparing,
    Installed,
    Failed
};

// Returns the report label for a lifecycle state, using Failed for an unrecognized value.
std::string_view state_name(PatchState state);

// Retains an admitted plugin and the configuration result shared by all its patches.
struct PluginRecord {
    AdmittedPlugin admitted;
    std::optional<PluginConfiguration> configuration;
    std::optional<Diagnostic> failure;
};

// Tracks one patch's selection, dependencies, handler and resources throughout startup and installation.
// Its address stays stable because callbacks and native resources use the record pointer as the patch identity.
struct PatchRecord {
    PatchRecord();
    PatchRecord(const PatchRecord &) = delete;
    PatchRecord &operator=(const PatchRecord &) = delete;
    PatchRecord(PatchRecord &&) = delete;
    PatchRecord &operator=(PatchRecord &&) = delete;
    // Invokes a supplied destroy callback for an unexposed handler; installed or exposed handlers remain alive until process exit.
    ~PatchRecord();

    PluginRecord *plugin{};
    const FC_Patch *definition{};
    // Creates a runtime Game or Galaxy handler through the patch lifecycle.
    FC_HandlerFactory internal_factory{};
    ResolvedPatch resolved;
    PatchSelection selection;
    bool enabled{};
    // Marks patches subject to startup failure policy: enabled Ready or Waiting patches,
    // and mandatory compatible patches whose configuration failed.
    bool selected_for_startup{};
    // Publishing Installed makes initialized handler and API state visible to callbacks on other threads.
    std::atomic<PatchState> state{PatchState::Disabled};
    std::string reason;
    std::vector<PatchRecord *> providers;
    FC_Handler handler{};
    std::unique_ptr<SubmittedPlan> plan;
    std::unique_ptr<PatchApiReservations> api;
    // Supplies this patch's logging, tracing and API facilities for the process lifetime.
    CoreRuntime *runtime{};
    std::chrono::steady_clock::time_point last_update{};
    std::chrono::steady_clock::time_point next_update{};

    // Returns the selected variant's policy, the patch default before selection, or Continue for runtime integrations.
    std::uint32_t failure_policy() const noexcept;

    // Invokes a supplied destroy callback and clears the handler only before Installed or native exposure.
    // Callback faults terminate the process.
    void destroy_unexposed() noexcept;
};

// Returns the runtime handler's diagnostic name, or Plugin.Patch with the spelling from its declarations.
std::string qualified_patch_id(const PatchRecord &patch);

// Owns plugin and patch records plus the accumulated selection and startup diagnostics.
struct SelectionSession {
    std::span<const ImageProfile> catalog;
    FC_Role role;
    std::vector<std::unique_ptr<PluginRecord>> plugins;
    std::vector<std::unique_ptr<PatchRecord>> patches;
    std::vector<Diagnostic> diagnostics;
    bool fatal{};
};

// Builds patch records and resolves their image selectors. Declaration storage and the catalog must outlive the session.
SelectionSession resolve_patches(AdmissionResult admitted, std::span<const ImageProfile> catalog, FC_Role role);

// Captures each plugin's startup configuration and selects patches on the control thread using those observations.
void configure_patches(SelectionSession &session, std::span<const ImageObservation> observations, const ConfigurationPaths &paths);

// Rechecks image availability and dependencies on the control thread using captured startup settings.
// Installed and Failed patches keep their final states; observations must use the session's catalog.
void refresh_selection(SelectionSession &session, std::span<const ImageObservation> observations);

// Creates a handler on the control thread; requires Planning and a runtime factory or a Ready variant with captured settings.
// The factory borrows settings for this call; failure returns a Diagnostic, marks Failed and invokes any supplied destroy callback.
std::expected<void, Diagnostic> construct_handler(PatchRecord &patch, const FC_Host *host);

// Terminates when the session records a fatal startup failure, naming a selected Failed fatal patch when available.
void enforce_startup_policy(const SelectionSession &session);

} // namespace fc::runtime
