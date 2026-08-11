#pragma once

#include <FusionCutter/target.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string_view>

namespace fusioncutter::reporting {

struct CrashReporterError {
    std::string_view detail;
    std::uint32_t windows_error;
};

enum class CorePhase : std::uint32_t {
    Startup,
    TargetRecognition,
    Configuration,
    PatchSelection,
    PatchPlanning,
    PatchValidation,
    PatchCommit,
    PatchActivation,
    Runtime,
    Completed,
    Failed,
};

enum class ExecutableRegionKind : std::uint32_t {
    PatchSite,
    Hook,
    Trampoline,
    Relay,
    CoreAllocation,
};

[[nodiscard]] std::expected<void, CrashReporterError> install_crash_reporter(HostRole role,
                                                                             std::string_view selected_proxy) noexcept;

void uninstall_crash_reporter() noexcept;

void publish_crash_target(const TargetContext& target, std::string_view fingerprint) noexcept;
void publish_crash_phase(CorePhase phase, const char* current_patch = nullptr) noexcept;
void publish_installed_patch(const char* patch_id) noexcept;

[[nodiscard]] bool publish_executable_region(std::uintptr_t address, std::size_t size, const char* owner,
                                             ExecutableRegionKind kind) noexcept;

void begin_expected_fault() noexcept;
void end_expected_fault() noexcept;

class ExpectedFaultScope {
  public:
    ExpectedFaultScope() noexcept {
        begin_expected_fault();
    }

    ~ExpectedFaultScope() {
        end_expected_fault();
    }

    ExpectedFaultScope(const ExpectedFaultScope&) = delete;
    ExpectedFaultScope& operator=(const ExpectedFaultScope&) = delete;
};

} // namespace fusioncutter::reporting
