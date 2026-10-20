#pragma once

#include "game.hpp"
#include "lobby.hpp"
#include "multiplayer.hpp"
#include "native/hook_registry.hpp"
#include "patches/patch_record.hpp"
#include "reporting/crash_reporter.hpp"
#include "reporting/reporting.hpp"
#include "reporting/tracing.hpp"

#include <condition_variable>
#include <expected>
#include <optional>
#include <map>
#include <mutex>
#include <span>
#include <vector>

namespace fc::runtime {

class PatchApiReservations;

// Owns plugin startup, patch installation and reporting for the process.
// One control thread runs lifecycle callbacks, updates and status; native callbacks can run on other threads.
class CoreRuntime final {
  public:
    // Binds runtime API tables to this owner; the borrowed catalog and runtime must survive until process exit after start.
    explicit CoreRuntime(std::span<const ImageProfile> catalog);
    CoreRuntime(const CoreRuntime &) = delete;
    CoreRuntime &operator=(const CoreRuntime &) = delete;

    // Call outside loader lock; returns 1 on success or 0 with an optional synchronous error on the caller's thread.
    // Concurrent and repeated calls share one startup result; another role or runtime DLL copy fails.
    std::uint32_t start(FC_Role, FC_ErrorSink);

    // Finishes traces before draining logs so ETW finalization failures reach disk; repeated calls wait for completion.
    // Call outside reporting workers; native hooks, services and reporting objects remain alive until process exit.
    void finish_reporting() noexcept;

    // Borrows selection records for control-thread work; their storage lasts until process exit after start().
    const SelectionSession &selection_session() const noexcept {
        return session_;
    }

    reporting::ReportingSession &reporting() noexcept {
        return reporting_;
    }

    TraceSession &traces() noexcept {
        return traces_;
    }

    CrashReporter &crash() noexcept {
        return crash_;
    }

    GameContext &game() noexcept {
        return game_;
    }

    MultiplayerService &multiplayer() noexcept {
        return multiplayer_;
    }

    LobbyService &lobby() noexcept {
        return lobby_;
    }

    // Supplies the runtime API tables to handlers; the tables remain valid for this runtime's lifetime.
    const FC_Host &host() const noexcept {
        return host_;
    }

  private:
    // Runs startup and repeated lifecycle work; recoverable startup failures complete the waiting start calls.
    void control_main() noexcept;

    // Initializes reporting, configuration, selection and initial patch installation on the control thread.
    void initialize();

    // Installs patches when their images load, runs updates and delivers callbacks and status on the control thread.
    void control_scan();

    // Selects the executable and Galaxy DLL profiles needed for messaging and lobby data.
    void select_galaxy_observer();

    // Refreshes Galaxy access, processes lobby requests and delivers service callbacks on the control thread.
    void service_control(std::chrono::steady_clock::time_point now);

    // Collects runtime and installed-handler status on the control thread, then submits the report for publication.
    void collect_status();

    // Copies a recognized, pinned image's identity and PE facts into crash reporting storage.
    void publish_image_fact(const LoadedImage &);

    // Submits newly accumulated session diagnostics on the control thread and advances the reporting cursor.
    void publish_diagnostics();

    // Reports changed startup choices and selection failures, skipping failures already retained as session diagnostics.
    void publish_selection();

    // Reports changes in the sampled game mode, session and availability on the control thread.
    void publish_game_state();

    // Records the control thread's startup result and wakes every caller waiting in start().
    void finish_start(std::expected<void, Diagnostic> result);

    // Records a fatal reason and the current consumer's identity on the failing thread before termination.
    static void fatal_handler(void *, std::string_view, std::uint32_t) noexcept;

    std::span<const ImageProfile> catalog_;
    FC_Role role_ = FC_ROLE_CLIENT;
    std::mutex start_mutex_;
    std::condition_variable start_finished_;
    // Protected by start_mutex_; started_ distinguishes an in-progress request from an owner not yet started.
    bool started_{};
    // Protected by start_mutex_; presence publishes completion, with either success or its diagnostic.
    std::optional<std::expected<void, Diagnostic>> startup_result_;
    // Named mutex retained to detect another runtime DLL in the same process.
    void *process_guard_{};
    ConfigurationPaths configuration_paths_;
    reporting::SessionFacts facts_;
    RuntimeSettings settings_;
    SelectionSession session_{{}, FC_ROLE_CLIENT};
    LiveImageObservations images_;
    HookRegistry hooks_;
    FC_ReportingApi reporting_table_{};
    FC_Host host_{};
    reporting::ReportingSession reporting_;
    TraceSession traces_;
    CrashReporter crash_;
    GameContext game_;
    MultiplayerService multiplayer_;
    LobbyService lobby_;

    // These handlers must be destroyed before the services they use. Installed handlers live until process exit.
    PatchRecord game_observer_;
    PatchRecord galaxy_observer_;
    std::chrono::steady_clock::time_point next_status_{};
    // Number of session diagnostics already submitted to reporting.
    std::size_t diagnostic_count_{};
    // Last presented selection message for each stable patch record; unchanged waiting states remain quiet.
    std::map<const PatchRecord *, std::string> selection_messages_;
    std::optional<GameState> logged_game_;
    bool game_logged_{};
};

// Starts the retained process-lifetime runtime or returns its recorded result, with the same contract as start().
std::uint32_t start_runtime(FC_Role role, FC_ErrorSink error);

} // namespace fc::runtime
