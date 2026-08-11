#pragma once

#include <FusionCutter/LoaderApi.h>

#include <Windows.h>

#include <expected>
#include <filesystem>
#include <string>
#include <string_view>

namespace fusioncutter::loaders {

struct CoreLoadError {
    std::string message;
    DWORD windows_error;
};

struct CoreConnection {
    HMODULE module;
    FC_CoreApi api;
    FC_InitializeResult initialization_result;
};

[[nodiscard]] std::expected<std::filesystem::path, CoreLoadError> loader_directory(HMODULE loader_module);

[[nodiscard]] std::expected<CoreConnection, CoreLoadError> initialize_core(HMODULE loader_module, FC_HostRole role,
                                                                           FC_LoaderStartupInfo startup_info) noexcept;

void write_fallback_status(
    HMODULE loader_module, FC_HostRole role, std::string_view reason, DWORD windows_error = ERROR_SUCCESS,
    std::string_view action =
        "FusionCutter.dll did not initialize. Verify that the adjacent core matches this loader.") noexcept;

} // namespace fusioncutter::loaders
