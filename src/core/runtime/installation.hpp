#pragma once

#include "patch_runtime.hpp"
#include "../core_logger.hpp"
#include "../reporting/crash_reporter.hpp"

#include <FusionCutter/PluginApi.h>

namespace fc::runtime {

// Return Fatal after local cleanup or ownership transfer for native effects that remain exposed.
enum class InstallationResult {
    Completed,
    Fatal,
};

// Supplies reporting, tracing, and memory capabilities scoped to the phase and owned outside the common installer.
struct InstallationServices {
    FC_ReportToken prepare_report{};
    FC_ReportToken activate_report{};
    void* context{};
    TraceSession* traces{};
    patching::NativeMemoryWriter memory_writer{patching::system_memory_writer()};
    // Framework lifecycle diagnostics share the backend while memory mutation stays behind its dedicated writer.
    CoreLogger logger;
};

// Runs ready work through the deterministic installer; tests may inject services without production reporters.
[[nodiscard]] InstallationResult install_ready_patches(const planning::InstallationPlan& plan,
                                                       PatchRuntimeState& runtime, InstallationServices services = {});

// Shares reporters between startup and late image installation.
[[nodiscard]] InstallationResult install_ready_patches(const planning::InstallationPlan& plan,
                                                       PatchRuntimeState& runtime, TraceSession& traces,
                                                       CrashReporter& crash, InstallationServices services = {});

} // namespace fc::runtime
