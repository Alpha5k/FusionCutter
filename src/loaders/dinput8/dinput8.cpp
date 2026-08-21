#include "../common/startup.hpp"

#define DIRECTINPUT_VERSION 0x0800
#include <Windows.h>
#include <dinput.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>

namespace {

using DirectInput8CreateFn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
using DllCanUnloadNowFn = HRESULT(WINAPI*)();
using DllGetClassObjectFn = HRESULT(WINAPI*)(REFCLSID, REFIID, LPVOID*);
using DllRegisterServerFn = HRESULT(WINAPI*)();
using GetdfDIJoystickFn = LPCDIDATAFORMAT(WINAPI*)();

// One table represents all six shipping exports after the optional proxy has overlaid its supported subset.
struct DirectInputExports {
    DirectInput8CreateFn direct_input8_create{};
    DllCanUnloadNowFn dll_can_unload_now{};
    DllGetClassObjectFn dll_get_class_object{};
    DllRegisterServerFn dll_register_server{};
    DllRegisterServerFn dll_unregister_server{};
    GetdfDIJoystickFn getdf_di_joystick{};
};

// This process-lifetime record owns both forwarding modules and the loader facts copied into framework initialization.
struct ForwardingState {
    HMODULE system_module{};
    HMODULE chain_module{};
    DirectInputExports exports;
    FC_LoaderStartupInfo startup{.loader_kind = FC_LOADER_KIND_DINPUT8,
                                 .direct_input_chain = FC_DIRECT_INPUT_CHAIN_SYSTEM_ONLY};
    DWORD forwarding_error{ERROR_MOD_NOT_FOUND};
    bool ready{};
};

// FindFirstFile's sentinel handle is not a kernel null handle and therefore needs its own focused owner.
class FindHandle final {
  public:
    explicit FindHandle(HANDLE handle) noexcept : handle_(handle) {}
    FindHandle(const FindHandle&) = delete;
    FindHandle& operator=(const FindHandle&) = delete;
    ~FindHandle() {
        if (handle_ != INVALID_HANDLE_VALUE) {
            FindClose(handle_);
        }
    }
    [[nodiscard]] HANDLE get() const noexcept {
        return handle_;
    }

  private:
    HANDLE handle_{};
};

HMODULE loader_module{};
std::once_flag forwarding_once;
ForwardingState forwarding;
fc::loaders::StartupController startup;

// These resolution helpers build one complete system table and a sparse table of overrides for optional forwarding.
template <class Function> [[nodiscard]] Function resolve(HMODULE module, const char* name) noexcept {
    return reinterpret_cast<Function>(GetProcAddress(module, name));
}

[[nodiscard]] DirectInputExports resolve_exports(HMODULE module) noexcept {
    return {.direct_input8_create = resolve<DirectInput8CreateFn>(module, "DirectInput8Create"),
            .dll_can_unload_now = resolve<DllCanUnloadNowFn>(module, "DllCanUnloadNow"),
            .dll_get_class_object = resolve<DllGetClassObjectFn>(module, "DllGetClassObject"),
            .dll_register_server = resolve<DllRegisterServerFn>(module, "DllRegisterServer"),
            .dll_unregister_server = resolve<DllRegisterServerFn>(module, "DllUnregisterServer"),
            .getdf_di_joystick = resolve<GetdfDIJoystickFn>(module, "GetdfDIJoystick")};
}

[[nodiscard]] bool complete(const DirectInputExports& exports) noexcept {
    return exports.direct_input8_create != nullptr && exports.dll_can_unload_now != nullptr &&
           exports.dll_get_class_object != nullptr && exports.dll_register_server != nullptr &&
           exports.dll_unregister_server != nullptr && exports.getdf_di_joystick != nullptr;
}

// Filesystem stores UTF-8 basenames as char8_t; the ABI record copies the same bytes into its display-only field.
[[nodiscard]] std::string utf8_basename(const std::filesystem::path& path) {
    const auto value = path.filename().u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

void copy_selected_proxy(std::string_view basename) noexcept {
    auto size = std::min(basename.size(), std::size(forwarding.startup.selected_proxy_basename) - 1);
    // Back up over UTF-8 continuation bytes so display-only truncation never emits malformed text.
    while (size != 0 && size < basename.size() && (static_cast<unsigned char>(basename[size]) & 0xc0U) == 0x80U) {
        --size;
    }
    std::ranges::copy_n(basename.begin(), size, forwarding.startup.selected_proxy_basename);
    forwarding.startup.selected_proxy_basename[size] = '\0';
}

void load_system_direct_input() {
    // System32 is resolved explicitly so this proxy can never recurse into itself through ordinary DLL search.
    std::array<wchar_t, 32'768> system_directory{};
    const auto length = GetSystemDirectoryW(system_directory.data(), static_cast<UINT>(system_directory.size()));
    if (length == 0 || length >= system_directory.size()) {
        forwarding.forwarding_error = length == 0 ? GetLastError() : ERROR_INSUFFICIENT_BUFFER;
        return;
    }
    const auto path = std::filesystem::path{std::wstring_view{system_directory.data(), length}} / L"dinput8.dll";
    forwarding.system_module = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (forwarding.system_module == nullptr) {
        forwarding.forwarding_error = GetLastError();
        return;
    }
    // All six documented exports are required from the system fallback because every forwarding route may use it.
    forwarding.exports = resolve_exports(forwarding.system_module);
    if (!complete(forwarding.exports)) {
        FreeLibrary(forwarding.system_module);
        forwarding.system_module = nullptr;
        forwarding.exports = {};
        forwarding.forwarding_error = ERROR_PROC_NOT_FOUND;
        return;
    }
    forwarding.forwarding_error = ERROR_SUCCESS;
    forwarding.ready = true;
}

void overlay_chain_exports(const DirectInputExports& chain) noexcept {
    // DirectInput8Create is mandatory for an accepted chain; every other proxy export is an optional override.
    forwarding.exports.direct_input8_create = chain.direct_input8_create;
    if (chain.dll_can_unload_now != nullptr) {
        forwarding.exports.dll_can_unload_now = chain.dll_can_unload_now;
    }
    if (chain.dll_get_class_object != nullptr) {
        forwarding.exports.dll_get_class_object = chain.dll_get_class_object;
    }
    if (chain.dll_register_server != nullptr) {
        forwarding.exports.dll_register_server = chain.dll_register_server;
    }
    if (chain.dll_unregister_server != nullptr) {
        forwarding.exports.dll_unregister_server = chain.dll_unregister_server;
    }
    if (chain.getdf_di_joystick != nullptr) {
        forwarding.exports.getdf_di_joystick = chain.getdf_di_joystick;
    }
}

void load_optional_chain() {
    // Enumerate only adjacent dinput8_*.dll files and retain a candidate only when exactly one physical file matches.
    const auto directory = fc::loaders::loader_directory(loader_module);
    if (!directory) {
        forwarding.startup.direct_input_chain = FC_DIRECT_INPUT_CHAIN_INVALID;
        forwarding.startup.windows_error = directory.error().windows_error;
        return;
    }

    WIN32_FIND_DATAW file{};
    const FindHandle find{FindFirstFileW((*directory / L"dinput8_*.dll").c_str(), &file)};
    if (find.get() == INVALID_HANDLE_VALUE) {
        const auto error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND) {
            forwarding.startup.direct_input_chain = FC_DIRECT_INPUT_CHAIN_INVALID;
            forwarding.startup.windows_error = error;
        }
        return;
    }

    std::filesystem::path candidate;
    do {
        if ((file.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            continue;
        }
        if (forwarding.startup.wildcard_match_count != std::numeric_limits<std::uint32_t>::max()) {
            ++forwarding.startup.wildcard_match_count;
        }
        if (forwarding.startup.wildcard_match_count == 1) {
            candidate = *directory / file.cFileName;
        }
    } while (FindNextFileW(find.get(), &file));
    const auto find_error = GetLastError();
    if (find_error != ERROR_NO_MORE_FILES) {
        forwarding.startup.direct_input_chain = FC_DIRECT_INPUT_CHAIN_INVALID;
        forwarding.startup.windows_error = find_error;
        return;
    }
    // Zero files intentionally preserves forwarding only to the system DLL; ambiguity is reported and selects none.
    if (forwarding.startup.wildcard_match_count == 0) {
        return;
    }
    if (forwarding.startup.wildcard_match_count > 1) {
        forwarding.startup.direct_input_chain = FC_DIRECT_INPUT_CHAIN_AMBIGUOUS;
        forwarding.startup.windows_error = ERROR_MORE_DATA;
        return;
    }

    // Prepare the complete candidate before changing the system fallback table, so any failure selects no proxy.
    const auto basename = utf8_basename(candidate);
    const auto chain_module =
        LoadLibraryExW(candidate.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (chain_module == nullptr) {
        forwarding.startup.direct_input_chain = FC_DIRECT_INPUT_CHAIN_INVALID;
        forwarding.startup.windows_error = GetLastError();
        return;
    }
    const auto chain = resolve_exports(chain_module);
    if (chain.direct_input8_create == nullptr) {
        FreeLibrary(chain_module);
        forwarding.startup.direct_input_chain = FC_DIRECT_INPUT_CHAIN_INVALID;
        forwarding.startup.windows_error = ERROR_PROC_NOT_FOUND;
        return;
    }
    forwarding.chain_module = chain_module;
    overlay_chain_exports(chain);
    copy_selected_proxy(basename);
    forwarding.startup.direct_input_chain = FC_DIRECT_INPUT_CHAIN_LOADED;
    forwarding.startup.windows_error = ERROR_SUCCESS;
}

void initialize_forwarding() noexcept {
    try {
        // Final forwarding state is established before framework target recognition receives the chain outcome.
        load_system_direct_input();
        if (forwarding.ready) {
            load_optional_chain();
        } else {
            forwarding.startup.direct_input_chain = FC_DIRECT_INPUT_CHAIN_INVALID;
            forwarding.startup.windows_error = forwarding.forwarding_error;
        }
    } catch (...) {
        // A discovery exception invalidates only the optional chain; a complete system table remains usable.
        forwarding.forwarding_error = ERROR_GEN_FAILURE;
        forwarding.startup.direct_input_chain = FC_DIRECT_INPUT_CHAIN_INVALID;
        forwarding.startup.windows_error = ERROR_GEN_FAILURE;
    }
}

void ensure_forwarding() noexcept {
    try {
        std::call_once(forwarding_once, &initialize_forwarding);
    } catch (...) {
        forwarding.ready = false;
        forwarding.forwarding_error = ERROR_GEN_FAILURE;
    }
}

[[nodiscard]] HRESULT forwarding_failure() noexcept {
    const auto error = forwarding.forwarding_error == ERROR_SUCCESS ? ERROR_MOD_NOT_FOUND : forwarding.forwarding_error;
    return HRESULT_FROM_WIN32(error);
}

} // namespace

// The primary DirectInput entry is the client startup trigger; every other export remains forwarding-only.
extern "C" HRESULT WINAPI FC_DInput8_DirectInput8Create(HINSTANCE instance, DWORD version, REFIID interface_id,
                                                        LPVOID* output, LPUNKNOWN outer) noexcept {
    ensure_forwarding();
    static_cast<void>(startup.start(loader_module, FC_HOST_ROLE_CLIENT, forwarding.startup));
    return forwarding.ready ? forwarding.exports.direct_input8_create(instance, version, interface_id, output, outer)
                            : forwarding_failure();
}

extern "C" HRESULT WINAPI FC_DInput8_DllCanUnloadNow() noexcept {
    ensure_forwarding();
    return forwarding.ready ? forwarding.exports.dll_can_unload_now() : forwarding_failure();
}

extern "C" HRESULT WINAPI FC_DInput8_DllGetClassObject(REFCLSID class_id, REFIID interface_id,
                                                       LPVOID* output) noexcept {
    ensure_forwarding();
    return forwarding.ready ? forwarding.exports.dll_get_class_object(class_id, interface_id, output)
                            : forwarding_failure();
}

extern "C" HRESULT WINAPI FC_DInput8_DllRegisterServer() noexcept {
    ensure_forwarding();
    return forwarding.ready ? forwarding.exports.dll_register_server() : forwarding_failure();
}

extern "C" HRESULT WINAPI FC_DInput8_DllUnregisterServer() noexcept {
    ensure_forwarding();
    return forwarding.ready ? forwarding.exports.dll_unregister_server() : forwarding_failure();
}

extern "C" LPCDIDATAFORMAT WINAPI FC_DInput8_GetdfDIJoystick() noexcept {
    ensure_forwarding();
    return forwarding.ready ? forwarding.exports.getdf_di_joystick() : nullptr;
}

// Process attach records only the proxy identity; first exported use owns forwarding and framework initialization.
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void*) {
    if (reason == DLL_PROCESS_ATTACH) {
        loader_module = instance;
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}
