#include "startup.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <exception>
#include <fstream>
#include <format>
#include <utility>

namespace fc::loaders {
namespace {

inline constexpr std::wstring_view kCoreFilename = L"FusionCutter.dll";
inline constexpr std::wstring_view kStatusFilename = L"FusionCutter.txt";
inline constexpr DWORD kFatalExitCode = 0xD1;
inline constexpr DWORD kUpdateIntervalMilliseconds = 50;

using QueryCore = const FC_CoreApi*(FC_CALL*)(std::uint32_t) noexcept;

// Keeps Windows error attribution attached to the loader operation that actually failed.
[[nodiscard]] CoreLoadError load_error(std::string message, DWORD windows_error) {
    return {std::move(message), windows_error};
}

// Human-readable role and loader names are used only by the bounded pre-initialization fallback report.
[[nodiscard]] std::string_view role_name(FC_HostRole role) noexcept {
    return role == FC_HOST_ROLE_SERVER ? "Server" : "Client";
}

[[nodiscard]] std::string_view loader_name(FC_LoaderKind loader) noexcept {
    if (loader == FC_LOADER_KIND_DINPUT8) {
        return "DInput8";
    }
    if (loader == FC_LOADER_KIND_RCONSERVER) {
        return "RconServer";
    }
    if (loader == FC_LOADER_KIND_BATTLEFRONT2) {
        return "Battlefront2";
    }
    return "Unknown";
}

// The fallback owns its timestamp because no framework reporting session exists when negotiation fails.
[[nodiscard]] std::string local_start_time() {
    SYSTEMTIME local{};
    GetLocalTime(&local);
    TIME_ZONE_INFORMATION zone{};
    const auto state = GetTimeZoneInformation(&zone);
    LONG bias = zone.Bias;
    if (state == TIME_ZONE_ID_STANDARD) {
        bias += zone.StandardBias;
    } else if (state == TIME_ZONE_ID_DAYLIGHT) {
        bias += zone.DaylightBias;
    }
    const auto offset_minutes = -bias;
    const auto minute_component = offset_minutes < 0 ? -(offset_minutes % 60) : offset_minutes % 60;
    return std::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02} {:+03}:{:02}", local.wYear, local.wMonth, local.wDay,
                       local.wHour, local.wMinute, local.wSecond, offset_minutes / 60, minute_component);
}

void debug_output(std::string_view reason, DWORD windows_error) noexcept {
    try {
        std::string output{"Fusion Cutter loader: "};
        output.append(reason);
        if (windows_error != ERROR_SUCCESS) {
            output.append(" (Windows error ");
            output.append(std::to_string(windows_error));
            output.push_back(')');
        }
        output.append("\r\n");
        OutputDebugStringA(output.c_str());
    } catch (...) {
        OutputDebugStringA("Fusion Cutter loader reporting failed.\r\n");
    }
}

[[nodiscard]] std::expected<CoreConnection, CoreLoadError>
load_and_initialize_core(HMODULE loader_module, FC_HostRole role, FC_LoaderStartupInfo startup) noexcept {
    try {
        // Absolute adjacent loading prevents working-directory or arbitrary PATH state from selecting FusionCutter.dll.
        auto directory = loader_directory(loader_module);
        if (!directory) {
            return std::unexpected(std::move(directory.error()));
        }
        const auto path = *directory / kCoreFilename;
        const auto module =
            LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (module == nullptr) {
            return std::unexpected(load_error("FusionCutter.dll could not be loaded", GetLastError()));
        }

        // Negotiation failure releases only an unaccepted module; an accepted framework DLL stays mapped for life.
        const auto fail = [module](std::string message, DWORD windows_error) {
            FreeLibrary(module);
            return std::unexpected(load_error(std::move(message), windows_error));
        };
        // Before copying, accept only the exact generation query and complete required callback table prefix.
        const auto query = reinterpret_cast<QueryCore>(GetProcAddress(module, "FusionCutter_QueryCore"));
        if (query == nullptr) {
            return fail("FusionCutter.dll does not export FusionCutter_QueryCore", ERROR_PROC_NOT_FOUND);
        }
        const auto* supplied = query(FC_CORE_ABI_GENERATION);
        constexpr auto required_size = offsetof(FC_CoreApi, update) + sizeof(supplied->update);
        if (supplied == nullptr) {
            return fail("FusionCutter.dll does not support this loader ABI", ERROR_REVISION_MISMATCH);
        }
        if (supplied->struct_size < required_size || supplied->initialize == nullptr || supplied->update == nullptr) {
            return fail("FusionCutter.dll returned a malformed loader API", ERROR_INVALID_DATA);
        }

        // The local known prefix remains valid independently of any future table tail in the retained framework DLL.
        FC_CoreApi api{
            .struct_size = sizeof(FC_CoreApi), .initialize = supplied->initialize, .update = supplied->update};
        const FC_InitializeArgs arguments{
            .struct_size = sizeof(FC_InitializeArgs), .host_role = role, .loader_startup = startup};
        return CoreConnection{module, api, api.initialize(&arguments)};
    } catch (...) {
        return std::unexpected(load_error("FusionCutter.dll loading ended unexpectedly", ERROR_GEN_FAILURE));
    }
}

} // namespace

std::expected<std::filesystem::path, CoreLoadError> loader_directory(HMODULE loader_module) {
    if (loader_module == nullptr) {
        return std::unexpected(load_error("The loader module is unavailable", ERROR_INVALID_HANDLE));
    }
    std::array<wchar_t, 32'768> module_path{};
    const auto length = GetModuleFileNameW(loader_module, module_path.data(), static_cast<DWORD>(module_path.size()));
    if (length == 0 || length >= module_path.size()) {
        return std::unexpected(load_error("The loader module path is unavailable",
                                          length == 0 ? GetLastError() : ERROR_INSUFFICIENT_BUFFER));
    }
    return std::filesystem::path{std::wstring_view{module_path.data(), length}}.parent_path();
}

void write_fallback_status(HMODULE loader_module, FC_HostRole role, const FC_LoaderStartupInfo& startup,
                           std::string_view operation, std::string_view reason, DWORD windows_error) noexcept {
    try {
        auto directory = loader_directory(loader_module);
        if (!directory) {
            debug_output(directory.error().message, directory.error().windows_error);
            return;
        }
        std::ofstream output(*directory / kStatusFilename, std::ios::binary | std::ios::trunc);
        if (!output) {
            debug_output("The fallback status file could not be opened", GetLastError());
            return;
        }
        // The fallback states only loader-owned facts and never invents target, plugin, or patch results.
        output << "Fusion Cutter loader " << FC_VERSION_STRING;
        if (std::string_view{FC_BUILD_ID} != "" && std::string_view{FC_BUILD_ID} != FC_VERSION_STRING) {
            output << " (build " << FC_BUILD_ID << ")";
        }
        output << "\r\n"
               << "  Started: " << local_start_time() << "\r\n"
               << "  Initialization: Unavailable\r\n"
               << "  Loader: " << loader_name(startup.loader_kind) << "\r\n"
               << "  Role: " << role_name(role) << "\r\n"
               << "  Architecture: " << (sizeof(void*) == 8 ? "x64" : "x86") << "\r\n"
               << "  Operation: " << operation << "\r\n"
               << "  Reason: " << reason << "\r\n";
        if (windows_error != ERROR_SUCCESS) {
            output << "  Windows error: " << windows_error << "\r\n";
        }
        output.flush();
        if (!output) {
            debug_output("The fallback status file could not be written", GetLastError());
        }
    } catch (...) {
        debug_output(reason, windows_error == ERROR_SUCCESS ? ERROR_GEN_FAILURE : windows_error);
    }
}

StartupDisposition StartupController::start(HMODULE loader_module, FC_HostRole role,
                                            FC_LoaderStartupInfo startup) noexcept {
    try {
        std::call_once(once_, [this, loader_module, role, startup] {
            // All exceptions are contained inside the one-time body so Initialize can never be retried.
            initialize_once(loader_module, role, startup);
        });
    } catch (...) {
        // initialize_once is nonthrowing; reaching this path indicates a standard-library integrity failure.
        write_fallback_status(loader_module, role, startup, "Start Fusion Cutter",
                              "The one-time loader startup guard failed", ERROR_GEN_FAILURE);
        TerminateProcess(GetCurrentProcess(), kFatalExitCode);
        std::terminate();
    }
    return disposition_;
}

void StartupController::initialize_once(HMODULE loader_module, FC_HostRole role,
                                        FC_LoaderStartupInfo startup) noexcept {
    // Compatibility failure alone unloads an unnegotiated FusionCutter.dll and continues host forwarding.
    auto connection = load_and_initialize_core(loader_module, role, startup);
    if (!connection) {
        write_fallback_status(loader_module, role, startup, "Load and negotiate FusionCutter.dll",
                              connection.error().message, connection.error().windows_error);
        disposition_ = StartupDisposition::CompatibilityFailure;
        return;
    }
    // Successful negotiation transfers the module and copied API into permanent loader-owned state.
    core_.emplace(std::move(*connection));
    if (core_->initialization_result == FC_INIT_FATAL) {
        // The framework has already attempted its authoritative fatal status and bounded flush of the startup log.
        TerminateProcess(GetCurrentProcess(), kFatalExitCode);
        std::terminate();
    }
    if (core_->initialization_result == FC_INIT_UNSUPPORTED) {
        disposition_ = StartupDisposition::Unsupported;
        return;
    }
    if (core_->initialization_result != FC_INIT_COMPLETED) {
        write_fallback_status(loader_module, role, startup, "Interpret framework initialization result",
                              "FusionCutter.dll returned an unknown initialization result", ERROR_INVALID_DATA);
        TerminateProcess(GetCurrentProcess(), kFatalExitCode);
        std::terminate();
    }

    // A Completed initialization result is published only after creating the permanent serialized pump.
    const auto thread = CreateThread(nullptr, 0, &pump_entry, this, 0, nullptr);
    if (thread == nullptr) {
        report_pump_failure_and_terminate(loader_module, role, startup, GetLastError());
    }
    CloseHandle(thread);
    pump_started_ = true;
    disposition_ = StartupDisposition::Completed;
}

DWORD WINAPI StartupController::pump_entry(void* context) noexcept {
    auto& self = *static_cast<StartupController*>(context);
    // The loader's permanent thread is the only serialized Update caller outside the game thread.
    for (;;) {
        Sleep(kUpdateIntervalMilliseconds);
        self.core_->api.update();
    }
}

[[noreturn]] void StartupController::report_pump_failure_and_terminate(HMODULE loader_module, FC_HostRole role,
                                                                       const FC_LoaderStartupInfo& startup,
                                                                       DWORD windows_error) noexcept {
    write_fallback_status(loader_module, role, startup, "Start serialized Update pump",
                          "Fusion Cutter initialization completed, but the required Update pump could not start",
                          windows_error);
    TerminateProcess(GetCurrentProcess(), kFatalExitCode);
    std::terminate();
}

bool StartupController::pump_started() const noexcept {
    return pump_started_;
}

} // namespace fc::loaders
