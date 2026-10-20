#pragma once

#include "patch_record.hpp"

#include <chrono>
#include <expected>
#include <span>
#include <vector>

namespace fc::runtime {

class HookRegistry;
struct PlanImage;
class CrashReporter;
class TraceSession;
class PatchApiReservations;

// Supplies runtime facilities needed to install patches on the control thread.
struct InstallationServices {
    const FC_Host *host{};
    HookRegistry *hooks{};
    CrashReporter *crash{};
    TraceSession *traces{};
    // Runtime game integrations, in installation order. Their records must remain alive while later patches install.
    std::span<PatchRecord *const> internal_native;
};

// Orders Planning patches with providers first, then by ASCII case-insensitive plugin and patch ID.
// Requires resolved acyclic dependencies; returned pointers borrow the session's records.
std::vector<PatchRecord *> ready_patch_order(const SelectionSession &session);

// Constructs a Planning handler and collects its plan; the caller owns cleanup and validation, and no preparation or writes occur.
// The host, owner, mapped storage, pinned modules and hooks stay alive while the handler and retained plan use them.
std::expected<void, Diagnostic> collect_patch_plan(PatchRecord &, const FC_Host *, std::vector<PlanImage>, HookRegistry *hooks = nullptr);

// Prepares, installs and activates Planning patches on the control thread; images stay pinned and services stay alive.
// Collects and validates plans before preparation; safely rejects failures or terminates after unsafe native changes.
void install_ready_patches(SelectionSession &, const LiveImageObservations &, InstallationServices);

// Adds a positive interval in milliseconds to now, saturating at the clock's last representable time.
std::chrono::steady_clock::time_point next_update_time(std::chrono::steady_clock::time_point now, std::int64_t interval_ms) noexcept;

// Calls each due installed handler once on the control thread and schedules its next update from now.
// Handler timestamps and elapsed times are nanoseconds measured with the steady clock.
void run_due_updates(SelectionSession &, std::chrono::steady_clock::time_point now);
// Notifies installed consumers of available patch APIs on the control thread.
void deliver_api_notifications(SelectionSession &);

} // namespace fc::runtime
