#pragma once

#include <FusionCutter/CoreApi.h>

#include <Windows.h>

#include <expected>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace fc::loaders {

// A compatibility failure keeps its actionable loader-owned cause paired with the originating Windows error.
struct CoreLoadError {
    std::string message;
    DWORD windows_error{};
};

// Successful negotiation retains FusionCutter.dll and the copied known prefix of its callback table for process life.
struct CoreConnection {
    HMODULE module{};
    FC_CoreApi api{};
    FC_InitializeResult initialization_result{};
};

// The stable startup outcome tells each proxy whether the framework and permanent pump were accepted.
enum class StartupDisposition {
    NotRun,
    CompatibilityFailure,
    Completed,
    Unsupported,
};

// Resolves the directory containing the specific shipping loader rather than relying on process working directory.
[[nodiscard]] std::expected<std::filesystem::path, CoreLoadError> loader_directory(HMODULE loader_module);

// Loader fallback is reserved for failures before the initialized framework can publish its own status.
void write_fallback_status(HMODULE loader_module, FC_HostRole role, const FC_LoaderStartupInfo& startup,
                           std::string_view operation, std::string_view reason,
                           DWORD windows_error = ERROR_SUCCESS) noexcept;

// One instance per shipping loader serializes framework API negotiation and owns the permanent 50 ms Update pump.
class StartupController final {
  public:
    StartupController() = default;
    StartupController(const StartupController&) = delete;
    StartupController& operator=(const StartupController&) = delete;

    // Fatal initialization and pump creation failure do not return; other results remain stable across calls.
    [[nodiscard]] StartupDisposition start(HMODULE loader_module, FC_HostRole role,
                                           FC_LoaderStartupInfo startup) noexcept;

    [[nodiscard]] bool pump_started() const noexcept;

  private:
    static DWORD WINAPI pump_entry(void* context) noexcept;
    void initialize_once(HMODULE loader_module, FC_HostRole role, FC_LoaderStartupInfo startup) noexcept;
    [[noreturn]] void report_pump_failure_and_terminate(HMODULE loader_module, FC_HostRole role,
                                                        const FC_LoaderStartupInfo& startup,
                                                        DWORD windows_error) noexcept;

    std::once_flag once_;
    std::optional<CoreConnection> core_;
    StartupDisposition disposition_{StartupDisposition::NotRun};
    bool pump_started_{};
};

} // namespace fc::loaders
