#include "crash_reporter.hpp"
#include "session.hpp"

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>

namespace {

#if defined(_M_IX86)
constexpr auto kArchitecture = fusioncutter::Architecture::X86;
#else
constexpr auto kArchitecture = fusioncutter::Architecture::X64;
#endif

class LiveStatus final : public fusioncutter::StatusContributor {
  public:
    enum class State {
        Idle,
        Connected,
    };

    void write_status(fusioncutter::StatusSection& output) const noexcept override {
        const auto state = state_.load(std::memory_order_relaxed);
        output.add("State", state == State::Idle ? "Idle" : "Connected");
        output.add("Route", "Direct");
    }

    void set_state(State state) noexcept {
        state_.store(state, std::memory_order_relaxed);
    }

  private:
    std::atomic<State> state_{State::Idle};
};

LiveStatus g_live_status;

class BoundedStatus final : public fusioncutter::StatusContributor {
  public:
    BoundedStatus() {
        long_value_.fill('X');
    }

    void write_status(fusioncutter::StatusSection& output) const noexcept override {
        output.add("Long", std::string_view(long_value_.data(), long_value_.size()));
        for (const auto label : kLabels) {
            output.add(label, "Value");
        }
        output.add("Overflow", "Must not be rendered");
    }

  private:
    static constexpr std::array<std::string_view, 11> kLabels{
        "Field1", "Field2", "Field3", "Field4", "Field5", "Field6", "Field7", "Field8", "Field9", "Field10", "Field11",
    };
    std::array<char, 400> long_value_{};
};

BoundedStatus g_bounded_status;

[[nodiscard]] std::filesystem::path executable_directory() {
    std::wstring path(32'768, L'\0');
    const auto size = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (size == 0 || size >= path.size()) {
        return {};
    }
    path.resize(size);
    return std::filesystem::path(std::move(path)).parent_path();
}

[[nodiscard]] std::string read_text(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return {};
    }
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void publish_test_status(fusioncutter::LogLevel level) {
    auto& reporting = fusioncutter::reporting::Session::instance();
    reporting.start(fusioncutter::HostRole::Client);
    reporting.set_level(level);
    reporting.set_target({
        fusioncutter::TargetLayout::SteamRetail,
        fusioncutter::HostRole::Client,
        {fusioncutter::TargetImage::Game, kArchitecture, 0x00400000, 0x00800000},
    });
    reporting.set_configuration(executable_directory() / L"FusionCutter.ini");

    const std::array results{
        fusioncutter::PatchResult{"DLCMissionLimit", "DLC Mission Limit", fusioncutter::PatchOutcome::Installed, {}},
        fusioncutter::PatchResult{
            "AimAssist", "Aim Assist", fusioncutter::PatchOutcome::Failed,
            fusioncutter::OutcomeReason{"expected instruction bytes did not match", "Install input hook", {}}},
        fusioncutter::PatchResult{"DisabledPatch", "Disabled Patch", fusioncutter::PatchOutcome::Disabled, {}},
        fusioncutter::PatchResult{"WrongTarget", "Wrong Target", fusioncutter::PatchOutcome::NotApplicable, {}},
    };
    const std::array contributors{
        fusioncutter::reporting::StatusContributorRef{"Direct Transport", &g_live_status},
    };
    reporting.publish_status({fusioncutter::InitializationOutcome::Completed, {}}, results, contributors);
}

[[nodiscard]] int run_ordinary_reporting(std::wstring_view mode) {
    if (mode == L"ordinary") {
        publish_test_status(fusioncutter::LogLevel::Warning);
        if (!fusioncutter::logging::enabled(fusioncutter::LogLevel::Error) ||
            !fusioncutter::logging::enabled(fusioncutter::LogLevel::Warning) ||
            fusioncutter::logging::enabled(fusioncutter::LogLevel::Info)) {
            return ERROR_INVALID_STATE;
        }
        fusioncutter::logging::info("DirectTransport", "filtered informational record");
        fusioncutter::logging::error("AimAssist", "expected instruction bytes did not match", "Install input hook");
        fusioncutter::reporting::Session::instance().flush();
        publish_test_status(fusioncutter::LogLevel::Warning);
        return ERROR_SUCCESS;
    }
    if (mode == L"off") {
        publish_test_status(fusioncutter::LogLevel::Off);
        if (fusioncutter::logging::enabled(fusioncutter::LogLevel::Error)) {
            return ERROR_INVALID_STATE;
        }
        fusioncutter::logging::error("Core", "this record must remain filtered");
        fusioncutter::reporting::Session::instance().flush();
        return ERROR_SUCCESS;
    }
    if (mode == L"rotation") {
        publish_test_status(fusioncutter::LogLevel::Error);
        fusioncutter::logging::error("Core", "rotation marker");
        fusioncutter::reporting::Session::instance().flush();
        return ERROR_SUCCESS;
    }
    if (mode == L"status-output-failure") {
        publish_test_status(fusioncutter::LogLevel::Off);
        return ERROR_SUCCESS;
    }
    if (mode == L"live-status") {
        publish_test_status(fusioncutter::LogLevel::Off);
        const auto status_path = executable_directory() / L"FusionCutter.txt";
        std::error_code error;
        const auto initial_time = std::filesystem::last_write_time(status_path, error);
        if (error) {
            return ERROR_FILE_NOT_FOUND;
        }

        Sleep(1'200);
        if (std::filesystem::last_write_time(status_path, error) != initial_time || error) {
            return ERROR_WRITE_FAULT;
        }

        g_live_status.set_state(LiveStatus::State::Connected);
        Sleep(1'200);
        if (std::filesystem::last_write_time(status_path, error) == initial_time || error ||
            !read_text(status_path).contains("State: Connected")) {
            return ERROR_WRITE_FAULT;
        }
        return ERROR_SUCCESS;
    }
    if (mode == L"bounded-status") {
        publish_test_status(fusioncutter::LogLevel::Off);
        const std::array contributors{
            fusioncutter::reporting::StatusContributorRef{"Bounded Feature", &g_bounded_status},
        };
        fusioncutter::reporting::Session::instance().publish_status(
            {fusioncutter::InitializationOutcome::Completed, {}}, {}, contributors);
        return ERROR_SUCCESS;
    }
    return ERROR_INVALID_FUNCTION;
}

void publish_test_metadata() {
    const auto module = GetModuleHandleW(nullptr);
    const auto base = reinterpret_cast<std::uintptr_t>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const auto image_size = static_cast<std::size_t>(nt->OptionalHeader.SizeOfImage);

    const fusioncutter::TargetContext target{
        fusioncutter::TargetLayout::SteamRetail,
        fusioncutter::HostRole::Client,
        {fusioncutter::TargetImage::Game, kArchitecture, base, image_size},
    };
    fusioncutter::reporting::publish_crash_target(target, "reporting-test-target");
    fusioncutter::reporting::publish_crash_phase(fusioncutter::reporting::CorePhase::PatchCommit, "test_patch");
    fusioncutter::reporting::publish_installed_patch("installed_test_patch");
    static_cast<void>(fusioncutter::reporting::publish_executable_region(
        base, image_size, "test_patch", fusioncutter::reporting::ExecutableRegionKind::Hook));
}

void raise_handled(std::uint32_t code) {
    ULONG_PTR parameters[2] = {0, 1};
    __try {
        RaiseException(code, 0, 2, parameters);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

[[noreturn]] void raise_unhandled() {
    const ULONG_PTR parameters[2] = {0, 1};
    RaiseException(EXCEPTION_ACCESS_VIOLATION, EXCEPTION_NONCONTINUABLE, 2, parameters);
    ExitProcess(ERROR_UNHANDLED_EXCEPTION);
}

} // namespace

int wmain(int argument_count, wchar_t** arguments) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    if (argument_count != 2) {
        return ERROR_INVALID_PARAMETER;
    }

    const std::wstring_view mode = arguments[1];
    if (const auto result = run_ordinary_reporting(mode); result != ERROR_INVALID_FUNCTION) {
        return result;
    }

    const auto installed =
        fusioncutter::reporting::install_crash_reporter(fusioncutter::HostRole::Client, "reporting-test-proxy.dll");
    if (!installed.has_value()) {
        return static_cast<int>(installed.error().windows_error == ERROR_SUCCESS ? ERROR_GEN_FAILURE
                                                                                 : installed.error().windows_error);
    }
    publish_test_metadata();

    if (mode == L"sequence") {
        {
            const fusioncutter::reporting::ExpectedFaultScope expected_fault;
            raise_handled(EXCEPTION_INT_DIVIDE_BY_ZERO);
        }

        for (int index = 0; index < 5; ++index) {
            raise_handled(EXCEPTION_ACCESS_VIOLATION);
        }
        fusioncutter::reporting::uninstall_crash_reporter();
        return ERROR_SUCCESS;
    }
    if (mode == L"fatal") {
        raise_unhandled();
    }

    fusioncutter::reporting::uninstall_crash_reporter();
    return ERROR_INVALID_PARAMETER;
}
