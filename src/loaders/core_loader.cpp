#include "core_loader.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <fstream>
#include <utility>

namespace fusioncutter::loaders {
namespace {

constexpr std::wstring_view kCoreFilename = L"FusionCutter.dll";
constexpr std::wstring_view kClientStatusFilename = L"FusionCutter.txt";
constexpr std::wstring_view kServerStatusFilename = L"FusionCutter-Server.txt";

using QueryCoreApi = decltype(&FusionCutter_GetCoreApi);

[[nodiscard]] CoreLoadError load_error(std::string message, DWORD windows_error) {
    return {std::move(message), windows_error};
}

[[nodiscard]] std::expected<std::filesystem::path, CoreLoadError> adjacent_path(HMODULE loader_module,
                                                                                std::wstring_view filename) {
    auto directory = loader_directory(loader_module);
    if (!directory) {
        return std::unexpected(std::move(directory.error()));
    }
    return *directory / filename;
}

void debug_output(std::string_view message, DWORD windows_error) noexcept {
    try {
        std::string output = "Fusion Cutter loader: ";
        output.append(message);
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

} // namespace

std::expected<std::filesystem::path, CoreLoadError> loader_directory(HMODULE loader_module) {
    if (loader_module == nullptr) {
        return std::unexpected(load_error("the loader module is unavailable", ERROR_INVALID_HANDLE));
    }

    std::array<wchar_t, 32'768> module_path{};
    const auto length = GetModuleFileNameW(loader_module, module_path.data(), static_cast<DWORD>(module_path.size()));
    if (length == 0 || length >= module_path.size()) {
        return std::unexpected(load_error("the loader module path is unavailable",
                                          length == 0 ? GetLastError() : ERROR_INSUFFICIENT_BUFFER));
    }
    return std::filesystem::path(std::wstring_view{module_path.data(), length}).parent_path();
}

std::expected<CoreConnection, CoreLoadError> initialize_core(HMODULE loader_module, FC_HostRole role,
                                                             FC_LoaderStartupInfo startup_info) noexcept {
    try {
        auto core_path = adjacent_path(loader_module, kCoreFilename);
        if (!core_path) {
            return std::unexpected(std::move(core_path.error()));
        }

        const auto module = LoadLibraryExW(core_path->c_str(), nullptr,
                                           LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (module == nullptr) {
            return std::unexpected(load_error("FusionCutter.dll could not be loaded", GetLastError()));
        }

        const auto fail = [module](std::string message, DWORD windows_error) {
            FreeLibrary(module);
            return std::unexpected(load_error(std::move(message), windows_error));
        };

        const auto query = reinterpret_cast<QueryCoreApi>(GetProcAddress(module, "FusionCutter_GetCoreApi"));
        if (query == nullptr) {
            return fail("FusionCutter.dll does not export FusionCutter_GetCoreApi", ERROR_PROC_NOT_FOUND);
        }

        FC_CoreApi api{};
        const auto query_result = query(FC_ABI_GENERATION, sizeof(api), &api);
        if (query_result != FC_QUERY_OK) {
            return fail("FusionCutter.dll does not support this loader ABI", ERROR_REVISION_MISMATCH);
        }
        if (api.struct_size < sizeof(FC_CoreApi) || api.abi_generation != FC_ABI_GENERATION ||
            api.initialize == nullptr || api.update == nullptr || api.report_host_event == nullptr) {
            return fail("FusionCutter.dll returned an invalid loader API", ERROR_INVALID_DATA);
        }
        if ((api.supported_roles & role) == 0) {
            return fail("FusionCutter.dll was built without the requested host role", ERROR_NOT_SUPPORTED);
        }

        startup_info.struct_size = sizeof(startup_info);
        FC_InitializeArgs arguments{
            .struct_size = sizeof(FC_InitializeArgs),
            .host_role = role,
            .loader_startup = startup_info,
        };
        const auto result = api.initialize(&arguments);
        return CoreConnection{module, api, result};
    } catch (...) {
        return std::unexpected(load_error("core loading ended unexpectedly", ERROR_GEN_FAILURE));
    }
}

void write_fallback_status(HMODULE loader_module, FC_HostRole role, std::string_view reason, DWORD windows_error,
                           std::string_view action) noexcept {
    try {
        const auto filename = role == FC_HOST_ROLE_SERVER ? kServerStatusFilename : kClientStatusFilename;
        auto status_path = adjacent_path(loader_module, filename);
        if (!status_path) {
            debug_output(status_path.error().message, status_path.error().windows_error);
            return;
        }

        std::ofstream output(*status_path, std::ios::binary | std::ios::trunc);
        if (!output) {
            debug_output("the fallback status file could not be opened", GetLastError());
            return;
        }

        output << "Fusion Cutter\r\n"
               << "Status: Fatal\r\n"
               << "Role: " << (role == FC_HOST_ROLE_SERVER ? "Server" : "Client") << "\r\n"
               << "Reason: " << reason << "\r\n";
        if (windows_error != ERROR_SUCCESS) {
            output << "Windows error: " << windows_error << "\r\n";
        }
        output << action << "\r\n";
        output.close();
        if (!output) {
            debug_output("the fallback status file could not be written", GetLastError());
        }
    } catch (...) {
        debug_output(reason, windows_error == ERROR_SUCCESS ? ERROR_GEN_FAILURE : windows_error);
    }
}

} // namespace fusioncutter::loaders
