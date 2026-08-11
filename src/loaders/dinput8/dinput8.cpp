#include "core_loader.hpp"

#define DIRECTINPUT_VERSION 0x0800
#include <Windows.h>
#include <dinput.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace {

using DirectInput8CreateFn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
using DllCanUnloadNowFn = HRESULT(WINAPI*)();
using DllGetClassObjectFn = HRESULT(WINAPI*)(REFCLSID, REFIID, LPVOID*);
using DllRegisterServerFn = HRESULT(WINAPI*)();
using GetdfDIJoystickFn = LPCDIDATAFORMAT(WINAPI*)();

struct DirectInputExports {
    DirectInput8CreateFn direct_input8_create{};
    DllCanUnloadNowFn dll_can_unload_now{};
    DllGetClassObjectFn dll_get_class_object{};
    DllRegisterServerFn dll_register_server{};
    DllRegisterServerFn dll_unregister_server{};
    GetdfDIJoystickFn getdf_di_joystick{};
};

struct ForwardingState {
    HMODULE system_module{};
    HMODULE chain_module{};
    DirectInputExports exports;
    FC_LoaderStartupInfo startup_info{
        .struct_size = sizeof(FC_LoaderStartupInfo),
        .loader_kind = FC_LOADER_KIND_DINPUT8,
        .direct_input_chain_outcome = FC_DIRECT_INPUT_CHAIN_SYSTEM_ONLY,
    };
    std::string chain_candidate;
    DWORD error{ERROR_MOD_NOT_FOUND};
    bool ready{};
};

class FindHandle {
  public:
    explicit FindHandle(HANDLE handle) noexcept : handle_(handle) {}

    ~FindHandle() {
        if (handle_ != INVALID_HANDLE_VALUE) {
            FindClose(handle_);
        }
    }

    FindHandle(const FindHandle&) = delete;
    FindHandle& operator=(const FindHandle&) = delete;

    [[nodiscard]] HANDLE get() const noexcept {
        return handle_;
    }

  private:
    HANDLE handle_;
};

HMODULE g_loader_module{};
INIT_ONCE g_forwarding_once = INIT_ONCE_STATIC_INIT;
INIT_ONCE g_core_once = INIT_ONCE_STATIC_INIT;
ForwardingState g_forwarding;
std::optional<fusioncutter::loaders::CoreConnection> g_core;

template <typename Function> [[nodiscard]] Function resolve(HMODULE module, const char* name) noexcept {
    return reinterpret_cast<Function>(GetProcAddress(module, name));
}

[[nodiscard]] DirectInputExports resolve_exports(HMODULE module) noexcept {
    return {
        resolve<DirectInput8CreateFn>(module, "DirectInput8Create"),
        resolve<DllCanUnloadNowFn>(module, "DllCanUnloadNow"),
        resolve<DllGetClassObjectFn>(module, "DllGetClassObject"),
        resolve<DllRegisterServerFn>(module, "DllRegisterServer"),
        resolve<DllRegisterServerFn>(module, "DllUnregisterServer"),
        resolve<GetdfDIJoystickFn>(module, "GetdfDIJoystick"),
    };
}

[[nodiscard]] bool complete(const DirectInputExports& exports) noexcept {
    return exports.direct_input8_create != nullptr && exports.dll_can_unload_now != nullptr &&
           exports.dll_get_class_object != nullptr && exports.dll_register_server != nullptr &&
           exports.dll_unregister_server != nullptr && exports.getdf_di_joystick != nullptr;
}

[[nodiscard]] std::string path_name(const std::filesystem::path& path) {
    const auto value = path.filename().u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

void copy_selected_proxy(std::string_view basename) noexcept {
    auto& destination = g_forwarding.startup_info.selected_proxy_basename;
    const auto length = std::min(basename.size(), std::size(destination) - 1);
    std::ranges::copy_n(basename.begin(), length, destination);
    destination[length] = '\0';
}

void load_system_direct_input() {
    std::array<wchar_t, 32'768> system_directory{};
    const auto length = GetSystemDirectoryW(system_directory.data(), static_cast<UINT>(system_directory.size()));
    if (length == 0 || length >= system_directory.size()) {
        g_forwarding.error = length == 0 ? GetLastError() : ERROR_INSUFFICIENT_BUFFER;
        return;
    }

    const auto path = std::filesystem::path(std::wstring_view{system_directory.data(), length}) / L"dinput8.dll";
    g_forwarding.system_module = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (g_forwarding.system_module == nullptr) {
        g_forwarding.error = GetLastError();
        return;
    }

    g_forwarding.exports = resolve_exports(g_forwarding.system_module);
    if (!complete(g_forwarding.exports)) {
        FreeLibrary(g_forwarding.system_module);
        g_forwarding.system_module = nullptr;
        g_forwarding.exports = {};
        g_forwarding.error = ERROR_PROC_NOT_FOUND;
        return;
    }
    g_forwarding.error = ERROR_SUCCESS;
    g_forwarding.ready = true;
}

void apply_chain_exports(const DirectInputExports& chain) noexcept {
    g_forwarding.exports.direct_input8_create = chain.direct_input8_create;
    if (chain.dll_can_unload_now != nullptr) {
        g_forwarding.exports.dll_can_unload_now = chain.dll_can_unload_now;
    }
    if (chain.dll_get_class_object != nullptr) {
        g_forwarding.exports.dll_get_class_object = chain.dll_get_class_object;
    }
    if (chain.dll_register_server != nullptr) {
        g_forwarding.exports.dll_register_server = chain.dll_register_server;
    }
    if (chain.dll_unregister_server != nullptr) {
        g_forwarding.exports.dll_unregister_server = chain.dll_unregister_server;
    }
    if (chain.getdf_di_joystick != nullptr) {
        g_forwarding.exports.getdf_di_joystick = chain.getdf_di_joystick;
    }
}

void load_chain() {
    const auto directory = fusioncutter::loaders::loader_directory(g_loader_module);
    if (!directory) {
        g_forwarding.startup_info.direct_input_chain_outcome = FC_DIRECT_INPUT_CHAIN_INVALID;
        g_forwarding.startup_info.windows_error = directory.error().windows_error;
        return;
    }

    WIN32_FIND_DATAW file_data{};
    const auto search = *directory / L"dinput8_*.dll";
    const FindHandle find(FindFirstFileW(search.c_str(), &file_data));
    if (find.get() == INVALID_HANDLE_VALUE) {
        const auto error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND) {
            g_forwarding.startup_info.direct_input_chain_outcome = FC_DIRECT_INPUT_CHAIN_INVALID;
            g_forwarding.startup_info.windows_error = error;
        }
        return;
    }

    std::filesystem::path candidate;
    do {
        if ((file_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            continue;
        }
        if (g_forwarding.startup_info.wildcard_match_count != UINT32_MAX) {
            ++g_forwarding.startup_info.wildcard_match_count;
        }
        if (g_forwarding.startup_info.wildcard_match_count == 1) {
            candidate = *directory / file_data.cFileName;
        }
    } while (FindNextFileW(find.get(), &file_data));
    const auto find_error = GetLastError();

    if (find_error != ERROR_NO_MORE_FILES) {
        g_forwarding.startup_info.direct_input_chain_outcome = FC_DIRECT_INPUT_CHAIN_INVALID;
        g_forwarding.startup_info.windows_error = find_error;
        return;
    }
    if (g_forwarding.startup_info.wildcard_match_count == 0) {
        return;
    }
    if (g_forwarding.startup_info.wildcard_match_count > 1) {
        g_forwarding.startup_info.direct_input_chain_outcome = FC_DIRECT_INPUT_CHAIN_AMBIGUOUS;
        g_forwarding.startup_info.windows_error = ERROR_MORE_DATA;
        return;
    }

    g_forwarding.chain_candidate = path_name(candidate);
    g_forwarding.chain_module =
        LoadLibraryExW(candidate.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (g_forwarding.chain_module == nullptr) {
        g_forwarding.startup_info.direct_input_chain_outcome = FC_DIRECT_INPUT_CHAIN_INVALID;
        g_forwarding.startup_info.windows_error = GetLastError();
        return;
    }

    const auto chain_exports = resolve_exports(g_forwarding.chain_module);
    if (chain_exports.direct_input8_create == nullptr) {
        FreeLibrary(g_forwarding.chain_module);
        g_forwarding.chain_module = nullptr;
        g_forwarding.startup_info.direct_input_chain_outcome = FC_DIRECT_INPUT_CHAIN_INVALID;
        g_forwarding.startup_info.windows_error = ERROR_PROC_NOT_FOUND;
        return;
    }

    apply_chain_exports(chain_exports);
    copy_selected_proxy(g_forwarding.chain_candidate);
    g_forwarding.startup_info.direct_input_chain_outcome = FC_DIRECT_INPUT_CHAIN_LOADED;
    g_forwarding.startup_info.windows_error = ERROR_SUCCESS;
}

BOOL CALLBACK initialize_forwarding_once(INIT_ONCE*, void*, void**) noexcept {
    try {
        load_system_direct_input();
        if (g_forwarding.ready) {
            load_chain();
        } else {
            g_forwarding.startup_info.windows_error = g_forwarding.error;
        }
    } catch (...) {
        g_forwarding.ready = false;
        g_forwarding.error = ERROR_GEN_FAILURE;
        g_forwarding.startup_info.direct_input_chain_outcome = FC_DIRECT_INPUT_CHAIN_INVALID;
        g_forwarding.startup_info.windows_error = ERROR_GEN_FAILURE;
    }
    return TRUE;
}

void report_forwarding_status() noexcept {
    try {
        if (!g_core.has_value()) {
            return;
        }

        const auto report = g_core->api.report_host_event;
        if (!g_forwarding.ready) {
            const auto message =
                "System DirectInput forwarding failed with Windows error " + std::to_string(g_forwarding.error);
            report(FC_HOST_EVENT_ERROR, message.c_str());
            return;
        }

        switch (g_forwarding.startup_info.direct_input_chain_outcome) {
        case FC_DIRECT_INPUT_CHAIN_LOADED: {
            const auto message = "DirectInput chain loaded: " + g_forwarding.chain_candidate;
            report(FC_HOST_EVENT_INFO, message.c_str());
            break;
        }
        case FC_DIRECT_INPUT_CHAIN_INVALID: {
            const auto candidate =
                g_forwarding.chain_candidate.empty() ? "the matching proxy" : g_forwarding.chain_candidate;
            const auto message = "DirectInput chain " + candidate + " was ignored (Windows error " +
                                 std::to_string(g_forwarding.startup_info.windows_error) + ")";
            report(FC_HOST_EVENT_ERROR, message.c_str());
            break;
        }
        case FC_DIRECT_INPUT_CHAIN_AMBIGUOUS: {
            const auto message = std::to_string(g_forwarding.startup_info.wildcard_match_count) +
                                 " files match dinput8_*.dll; no DirectInput chain was selected";
            report(FC_HOST_EVENT_ERROR, message.c_str());
            break;
        }
        default:
            break;
        }
    } catch (...) {
        g_core->api.report_host_event(FC_HOST_EVENT_ERROR, "DirectInput forwarding status could not be reported");
    }
}

BOOL CALLBACK initialize_core_once(INIT_ONCE*, void*, void**) noexcept {
    auto connection =
        fusioncutter::loaders::initialize_core(g_loader_module, FC_HOST_ROLE_CLIENT, g_forwarding.startup_info);
    if (!connection) {
        fusioncutter::loaders::write_fallback_status(g_loader_module, FC_HOST_ROLE_CLIENT, connection.error().message,
                                                     connection.error().windows_error);
        return TRUE;
    }
    g_core.emplace(std::move(*connection));
    report_forwarding_status();
    return TRUE;
}

void ensure_forwarding() noexcept {
    InitOnceExecuteOnce(&g_forwarding_once, initialize_forwarding_once, nullptr, nullptr);
}

void ensure_core() noexcept {
    InitOnceExecuteOnce(&g_core_once, initialize_core_once, nullptr, nullptr);
}

[[nodiscard]] HRESULT forwarding_failure() noexcept {
    const auto error = g_forwarding.error == ERROR_SUCCESS ? ERROR_MOD_NOT_FOUND : g_forwarding.error;
    return HRESULT_FROM_WIN32(error);
}

} // namespace

extern "C" HRESULT WINAPI FC_DInput8_DirectInput8Create(HINSTANCE instance, DWORD version, REFIID interface_id,
                                                        LPVOID* output, LPUNKNOWN outer) noexcept {
    ensure_forwarding();
    ensure_core();
    if (!g_forwarding.ready) {
        return forwarding_failure();
    }
    return g_forwarding.exports.direct_input8_create(instance, version, interface_id, output, outer);
}

extern "C" HRESULT WINAPI FC_DInput8_DllCanUnloadNow() noexcept {
    ensure_forwarding();
    return g_forwarding.ready ? g_forwarding.exports.dll_can_unload_now() : forwarding_failure();
}

extern "C" HRESULT WINAPI FC_DInput8_DllGetClassObject(REFCLSID class_id, REFIID interface_id,
                                                       LPVOID* output) noexcept {
    ensure_forwarding();
    return g_forwarding.ready ? g_forwarding.exports.dll_get_class_object(class_id, interface_id, output)
                              : forwarding_failure();
}

extern "C" HRESULT WINAPI FC_DInput8_DllRegisterServer() noexcept {
    ensure_forwarding();
    return g_forwarding.ready ? g_forwarding.exports.dll_register_server() : forwarding_failure();
}

extern "C" HRESULT WINAPI FC_DInput8_DllUnregisterServer() noexcept {
    ensure_forwarding();
    return g_forwarding.ready ? g_forwarding.exports.dll_unregister_server() : forwarding_failure();
}

extern "C" LPCDIDATAFORMAT WINAPI FC_DInput8_GetdfDIJoystick() noexcept {
    ensure_forwarding();
    return g_forwarding.ready ? g_forwarding.exports.getdf_di_joystick() : nullptr;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void*) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_loader_module = instance;
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}
