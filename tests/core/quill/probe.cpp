#include "module_api.hpp"

#include <Windows.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace {

using namespace fusioncutter::tests::quill;

template <typename Function> [[nodiscard]] Function load_function(HMODULE module, const char* name) {
    return reinterpret_cast<Function>(GetProcAddress(module, name));
}

} // namespace

int wmain(int argument_count, wchar_t** arguments) {
    if (argument_count != 4) {
        return ERROR_INVALID_PARAMETER;
    }

    const std::wstring_view mode = arguments[1];
    const auto module = LoadLibraryW(arguments[2]);
    if (module == nullptr) {
        return static_cast<int>(GetLastError());
    }

    const auto start = load_function<StartFunction>(module, kStartName);
    const auto log = load_function<LogFunction>(module, kLogName);
    const auto saturate = load_function<SaturateFunction>(module, kSaturateName);
    const auto stop = load_function<StopFunction>(module, kStopName);
    if (start == nullptr || log == nullptr || saturate == nullptr || stop == nullptr) {
        return ERROR_PROC_NOT_FOUND;
    }

    const auto output_directory = std::filesystem::path(arguments[3]);
    const auto log_path = (output_directory / L"quill.log").string();
    const auto start_result = start(log_path.c_str(), mode == L"saturation");
    if (start_result != ERROR_SUCCESS) {
        return start_result;
    }

    log("Fusion Cutter Quill acceptance marker");
    if (mode == L"normal-exit") {
        return ERROR_SUCCESS;
    }
    if (mode == L"explicit-unload") {
        return FreeLibrary(module) ? ERROR_SUCCESS : static_cast<int>(GetLastError());
    }
    if (mode == L"saturation") {
        constexpr std::uint32_t kMessageCount = 250'000;
        const auto elapsed_milliseconds = saturate(kMessageCount);
        const auto notifications = stop();
        if (!FreeLibrary(module)) {
            return static_cast<int>(GetLastError());
        }
        if (elapsed_milliseconds > 5'000 || notifications == 0) {
            return ERROR_TIMEOUT;
        }
        return ERROR_SUCCESS;
    }

    return ERROR_INVALID_PARAMETER;
}
