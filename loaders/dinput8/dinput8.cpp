#include "../startup.hpp"

#define DIRECTINPUT_VERSION 0x0800
#include <Windows.h>
#include <dinput.h>

#include <algorithm>
#include <atomic>
#include <fstream>
#include <array>
#include <cstdint>
#include <filesystem>
#include <expected>
#include <stdexcept>
#include <mutex>
#include <string>
#include <string_view>

namespace {

using DirectInput8CreateFn = HRESULT(WINAPI *)(HINSTANCE, DWORD, REFIID, LPVOID *, LPUNKNOWN);
using DllCanUnloadNowFn = HRESULT(WINAPI *)();
using DllGetClassObjectFn = HRESULT(WINAPI *)(REFCLSID, REFIID, LPVOID *);
using DllRegisterServerFn = HRESULT(WINAPI *)();
using GetdfDIJoystickFn = LPCDIDATAFORMAT(WINAPI *)();

// Holds the six DirectInput exports used to forward calls to a loaded DLL.
struct DirectInputExports {
    DirectInput8CreateFn direct_input8_create{};
    DllCanUnloadNowFn dll_can_unload_now{};
    DllGetClassObjectFn dll_get_class_object{};
    DllRegisterServerFn dll_register_server{};
    DllRegisterServerFn dll_unregister_server{};
    GetdfDIJoystickFn getdf_di_joystick{};
};

// Stores the forwarding destinations and the result of their initialization.
struct ForwardingState {
    // Keep the system destinations available as a fallback if the optional proxy causes recursion.
    DirectInputExports system_exports, exports;
};

// Once recursion rejects a proxy, subsequent calls from every thread use the system DLL.
std::atomic_bool rejected_chain{};
// Detects a proxy calling back into this thread's DirectInput8Create invocation.
thread_local bool in_primary{};

// Closes a file enumeration with FindClose when it leaves scope.
class FindHandle final {
  public:
    explicit FindHandle(HANDLE handle) noexcept : handle_(handle) {}

    FindHandle(const FindHandle &) = delete;
    FindHandle &operator=(const FindHandle &) = delete;

    // Releases the enumeration handle when discovery finishes or exits with an error.
    ~FindHandle() {
        if (handle_ != INVALID_HANDLE_VALUE) {
            FindClose(handle_);
        }
    }

    HANDLE get() const noexcept {
        return handle_;
    }

  private:
    HANDLE handle_{};
};

HMODULE loader_module{};
std::once_flag forwarding_once;
std::expected<ForwardingState, DWORD> forwarding{std::unexpected(ERROR_MOD_NOT_FOUND)};
fc::loaders::StartupController startup;

// Looks up a named export with the caller's function type; returns null when the name is missing.
template <class Function> Function resolve(HMODULE module, const char *name) noexcept {
    return reinterpret_cast<Function>(GetProcAddress(module, name));
}

// Collect the DLL's supported exports; missing entries remain null.
DirectInputExports resolve_exports(HMODULE module) noexcept {
    return {.direct_input8_create = resolve<DirectInput8CreateFn>(module, "DirectInput8Create"),
            .dll_can_unload_now = resolve<DllCanUnloadNowFn>(module, "DllCanUnloadNow"),
            .dll_get_class_object = resolve<DllGetClassObjectFn>(module, "DllGetClassObject"),
            .dll_register_server = resolve<DllRegisterServerFn>(module, "DllRegisterServer"),
            .dll_unregister_server = resolve<DllRegisterServerFn>(module, "DllUnregisterServer"),
            .getdf_di_joystick = resolve<GetdfDIJoystickFn>(module, "GetdfDIJoystick")};
}

// Checks that all six forwarding destinations are available.
bool complete(const DirectInputExports &exports) noexcept {
    return exports.direct_input8_create != nullptr && exports.dll_can_unload_now != nullptr && exports.dll_get_class_object != nullptr &&
           exports.dll_register_server != nullptr && exports.dll_unregister_server != nullptr && exports.getdf_di_joystick != nullptr;
}

// Writes a proxy warning to the debugger, using a fixed message if constructing the warning fails.
void warn_chain(std::string_view reason) noexcept {
    try {
        const auto message = std::string{"Fusion Cutter DirectInput warning: "} + std::string{reason} + "\r\n";
        OutputDebugStringA(message.c_str());
    } catch (...) {
        OutputDebugStringA("Fusion Cutter DirectInput warning could not be formatted.\r\n");
    }
}

// GameExt redirects arrays initialized by the executable's constructors, so load it during process attachment.
// This matches its own proxy; Fusion Cutter itself starts later, outside loader lock.
void load_optional_game_ext() noexcept {
    try {
        if (GetModuleHandleW(L"BF2GameExt.dll")) {
            return;
        }
        const auto directory = fc::loaders::loader_directory(loader_module);
        if (!directory) {
            warn_chain("Could not determine the loader's directory for optional BF2GameExt.dll");
            return;
        }
        const auto path = *directory / L"BF2GameExt.dll";
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
            const auto error = GetLastError();
            if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) {
                warn_chain("Could not inspect optional BF2GameExt.dll");
            }
            return;
        }

        // Match GameExt's proxy protocol so DllMain defers initialization to the adjacent INI.
        const auto ini_path = (*directory / L"BF2GameExt.ini").string();
        if (!GetPrivateProfileIntA("General", "Enabled", 1, ini_path.c_str())) {
            return;
        }
        SetLastError(ERROR_SUCCESS);
        const bool set_marker = GetEnvironmentVariableW(L"BF2GAMEEXT_PROXY", nullptr, 0) == 0;
        const bool marker_missing = GetLastError() == ERROR_ENVVAR_NOT_FOUND;
        if (set_marker && !SetEnvironmentVariableW(L"BF2GAMEEXT_PROXY", L"1")) {
            warn_chain("Could not defer optional BF2GameExt.dll initialization");
            return;
        }
        // Retain successful initialization for the process lifetime because GameExt's patches can call into this DLL.
        const auto game_ext = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (set_marker && !SetEnvironmentVariableW(L"BF2GAMEEXT_PROXY", marker_missing ? nullptr : L"")) {
            warn_chain("Could not restore BF2GAMEEXT_PROXY after loading optional BF2GameExt.dll");
        }
        if (!game_ext) {
            warn_chain("Could not load optional BF2GameExt.dll");
            return;
        }

        using GameExtInitFn = BOOL(WINAPI *)(std::uintptr_t, const char *);
        const auto init = resolve<GameExtInitFn>(game_ext, "BF2GameExt_Init");
        if (!init || !init(reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)), ini_path.c_str())) {
            FreeLibrary(game_ext);
            warn_chain(init ? "Optional BF2GameExt.dll initialization failed" : "Optional BF2GameExt.dll does not export BF2GameExt_Init");
            return;
        }
    } catch (...) {
        warn_chain("Optional BF2GameExt.dll setup failed");
    }
}

// Detects this loader or an identical copy; failures to resolve paths or read files throw.
bool same_proxy_file(const std::filesystem::path &candidate) {
    std::array<wchar_t, 32768> path{};
    const auto length = GetModuleFileNameW(loader_module, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length >= path.size()) {
        throw std::runtime_error("Could not read the DirectInput loader's path");
    }

    const std::filesystem::path proxy{std::wstring_view{path.data(), length}};
    // File identity rejects hard links; byte identity also rejects an adjacent copy of this proxy.
    if (std::filesystem::equivalent(candidate, proxy)) {
        return true;
    }
    if (std::filesystem::file_size(candidate) != std::filesystem::file_size(proxy)) {
        return false;
    }

    std::ifstream a{candidate, std::ios::binary}, b{proxy, std::ios::binary};
    if (!a || !b) {
        throw std::runtime_error("Could not open the loader or optional DirectInput DLL for comparison");
    }

    // Compare equal-size files in chunks so detecting a copied proxy uses bounded memory.
    std::array<char, 65536> left{}, right{};
    while (a && b) {
        a.read(left.data(), static_cast<std::streamsize>(left.size()));
        b.read(right.data(), static_cast<std::streamsize>(right.size()));
        if (a.gcount() != b.gcount() || !std::equal(left.begin(), left.begin() + static_cast<std::ptrdiff_t>(a.gcount()), right.begin())) {
            return false;
        }
    }
    if (a.bad() || b.bad()) {
        throw std::runtime_error("Could not read the loader or optional DirectInput DLL for comparison");
    }
    return true;
}

// Loads the complete system forwarding table and retains its module, or returns the Windows error.
std::expected<DirectInputExports, DWORD> load_system_direct_input() {
    // Resolve System32 explicitly to load Windows' DirectInput DLL instead of this proxy.
    std::array<wchar_t, 32'768> system_directory{};
    const auto length = GetSystemDirectoryW(system_directory.data(), static_cast<UINT>(system_directory.size()));
    if (length == 0 || length >= system_directory.size()) {
        return std::unexpected(length == 0 ? GetLastError() : ERROR_INSUFFICIENT_BUFFER);
    }

    const auto path = std::filesystem::path{std::wstring_view{system_directory.data(), length}} / L"dinput8.dll";
    const auto system_module = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (system_module == nullptr) {
        return std::unexpected(GetLastError());
    }

    // All six documented exports are required from the system fallback because every forwarding route may use it.
    const auto exports = resolve_exports(system_module);
    if (!complete(exports)) {
        FreeLibrary(system_module);
        return std::unexpected(ERROR_PROC_NOT_FOUND);
    }

    // Retain the module reference for the process lifetime; exported calls keep using these pointers.
    return exports;
}

// Overrides system destinations with the proxy's exports; the proxy must supply DirectInput8Create.
// Missing secondary exports keep their system destinations.
void overlay_chain_exports(DirectInputExports &exports, const DirectInputExports &chain) noexcept {
    exports.direct_input8_create = chain.direct_input8_create;
    if (chain.dll_can_unload_now != nullptr) {
        exports.dll_can_unload_now = chain.dll_can_unload_now;
    }
    if (chain.dll_get_class_object != nullptr) {
        exports.dll_get_class_object = chain.dll_get_class_object;
    }
    if (chain.dll_register_server != nullptr) {
        exports.dll_register_server = chain.dll_register_server;
    }
    if (chain.dll_unregister_server != nullptr) {
        exports.dll_unregister_server = chain.dll_unregister_server;
    }
    if (chain.getdf_di_joystick != nullptr) {
        exports.getdf_di_joystick = chain.getdf_di_joystick;
    }
}

// Selects one adjacent proxy and overlays its accepted exports onto the complete system forwarding table.
// Call after system forwarding is ready; rejected candidates leave that table in use.
void load_optional_chain(DirectInputExports &exports) {
    // Look beside this loader for a single optional dinput8_*.dll proxy.
    const auto directory = fc::loaders::loader_directory(loader_module);
    if (!directory) {
        warn_chain("Could not determine the loader's directory; using Windows DirectInput");
        return;
    }

    WIN32_FIND_DATAW file{};
    const FindHandle find{FindFirstFileW((*directory / L"dinput8_*.dll").c_str(), &file)};
    if (find.get() == INVALID_HANDLE_VALUE) {
        const auto error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND) {
            warn_chain("Could not list dinput8_*.dll files beside the loader; using Windows DirectInput");
        }
        return;
    }

    std::filesystem::path candidate;
    std::size_t match_count{};
    do {
        if ((file.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            continue;
        }
        ++match_count;
        if (match_count == 1) {
            candidate = *directory / file.cFileName;
        }
    } while (FindNextFileW(find.get(), &file));
    const auto find_error = GetLastError();
    if (find_error != ERROR_NO_MORE_FILES) {
        warn_chain("Could not list dinput8_*.dll files beside the loader; using Windows DirectInput");
        return;
    }

    // Keep system forwarding when there is no candidate or the filenames leave the choice ambiguous.
    if (match_count == 0) {
        return;
    }
    if (match_count > 1) {
        warn_chain("Several dinput8_*.dll files were found beside the loader; using Windows DirectInput");
        return;
    }

    // Reject copies of this loader, then load and validate the candidate before using its exports.
    if (same_proxy_file(candidate)) {
        warn_chain("The optional DirectInput DLL is this loader or an identical copy; using Windows DirectInput");
        return;
    }

    const auto chain_module = LoadLibraryExW(candidate.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (chain_module == nullptr) {
        warn_chain("Could not load the optional DirectInput DLL; using Windows DirectInput");
        return;
    }

    const auto chain = resolve_exports(chain_module);
    if (chain.direct_input8_create == nullptr) {
        FreeLibrary(chain_module);
        warn_chain("The optional DirectInput DLL does not export DirectInput8Create; using Windows DirectInput");
        return;
    }

    // Forwarded exports can point into another DLL; reject entries that lead back to this loader.
    const std::array entries{reinterpret_cast<LPCWSTR>(chain.direct_input8_create),  reinterpret_cast<LPCWSTR>(chain.dll_can_unload_now),
                             reinterpret_cast<LPCWSTR>(chain.dll_get_class_object),  reinterpret_cast<LPCWSTR>(chain.dll_register_server),
                             reinterpret_cast<LPCWSTR>(chain.dll_unregister_server), reinterpret_cast<LPCWSTR>(chain.getdf_di_joystick)};
    for (const auto entry : entries) {
        if (entry == nullptr) {
            continue;
        }
        HMODULE entry_module{};
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, entry,
                                &entry_module) ||
            entry_module == loader_module) {
            FreeLibrary(chain_module);
            warn_chain("Could not verify that the optional DirectInput exports point outside this loader; using Windows DirectInput");
            return;
        }
    }

    // Keep the accepted proxy loaded for the lifetime of its function pointers.
    overlay_chain_exports(exports, chain);
}

// Builds complete forwarding tables, preserving the system fallback if optional proxy discovery throws.
std::expected<ForwardingState, DWORD> initialize_forwarding() noexcept {
    try {
        auto system = load_system_direct_input();
        if (!system) {
            return std::unexpected(system.error());
        }

        ForwardingState result{*system, *system};
        try {
            load_optional_chain(result.exports);
        } catch (...) {
            warn_chain("Optional DirectInput setup failed; using Windows DirectInput");
        }
        return result;
    } catch (...) {
        warn_chain("Windows DirectInput setup failed");
        return std::unexpected(ERROR_GEN_FAILURE);
    }
}

// Waits for initialization and selects the active table; failures remain local to this exported call.
std::expected<const DirectInputExports *, DWORD> active_exports() noexcept {
    try {
        std::call_once(forwarding_once, [] {
            forwarding = initialize_forwarding();
        });
    } catch (...) {
        return std::unexpected(ERROR_GEN_FAILURE);
    }
    if (!forwarding) {
        return std::unexpected(forwarding.error());
    }
    return rejected_chain.load(std::memory_order_acquire) ? &forwarding->system_exports : &forwarding->exports;
}

// Converts a forwarding failure to an HRESULT, retaining a failure result if Windows returned no error code.
HRESULT forwarding_failure(DWORD error) noexcept {
    return HRESULT_FROM_WIN32(error == ERROR_SUCCESS ? ERROR_MOD_NOT_FOUND : error);
}

} // namespace

// Call outside loader lock; starts Fusion Cutter on the first call, then forwards the request or returns a forwarding HRESULT.
// Runtime startup failures terminate the process through the loader's failure policy.
extern "C" HRESULT WINAPI FC_DInput8_DirectInput8Create(HINSTANCE instance, DWORD version, REFIID interface_id, LPVOID *output,
                                                        LPUNKNOWN outer) noexcept {
    const auto exports = active_exports();
    startup.start(loader_module, false, "DInput8");
    if (!exports) {
        return forwarding_failure(exports.error());
    }
    // Break a recursive proxy call by using the system DLL and retiring the optional proxy's exports.
    if (in_primary) {
        if (!rejected_chain.exchange(true, std::memory_order_acq_rel)) {
            warn_chain("This DirectInput loader was called recursively; using Windows DirectInput");
        }
        return forwarding->system_exports.direct_input8_create(instance, version, interface_id, output, outer);
    }

    // Keep recursion detection local to this thread and clear it on every return from the forwarded call.
    struct PrimaryScope {
        // Marks this thread as executing the primary forwarding route.
        PrimaryScope() {
            in_primary = true;
        }

        // Ends this thread's primary call so a later call can choose its forwarding destination normally.
        ~PrimaryScope() {
            in_primary = false;
        }
    } scope;

    // Startup may have waited while another thread rejected the optional proxy.
    const auto *target = rejected_chain.load(std::memory_order_acquire) ? &forwarding->system_exports : *exports;
    return target->direct_input8_create(instance, version, interface_id, output, outer);
}

// Forwards the COM unload query to the selected DLL or returns the recorded forwarding failure.
// Call secondary exports outside loader lock; they initialize forwarding without starting Fusion Cutter.
extern "C" HRESULT WINAPI FC_DInput8_DllCanUnloadNow() noexcept {
    const auto exports = active_exports();
    return exports ? (*exports)->dll_can_unload_now() : forwarding_failure(exports.error());
}

// Forwards the COM class-object request to the selected DLL or returns the recorded forwarding failure.
extern "C" HRESULT WINAPI FC_DInput8_DllGetClassObject(REFCLSID class_id, REFIID interface_id, LPVOID *output) noexcept {
    const auto exports = active_exports();
    return exports ? (*exports)->dll_get_class_object(class_id, interface_id, output) : forwarding_failure(exports.error());
}

// Forwards COM server registration to the selected DLL or returns the recorded forwarding failure.
extern "C" HRESULT WINAPI FC_DInput8_DllRegisterServer() noexcept {
    const auto exports = active_exports();
    return exports ? (*exports)->dll_register_server() : forwarding_failure(exports.error());
}

// Forwards COM server unregistration to the selected DLL or returns the recorded forwarding failure.
extern "C" HRESULT WINAPI FC_DInput8_DllUnregisterServer() noexcept {
    const auto exports = active_exports();
    return exports ? (*exports)->dll_unregister_server() : forwarding_failure(exports.error());
}

// Call outside loader lock; returns the selected DLL's joystick format or null if forwarding is unavailable.
// The format remains owned by that DLL, whose module reference is retained.
extern "C" LPCDIDATAFORMAT WINAPI FC_DInput8_GetdfDIJoystick() noexcept {
    const auto exports = active_exports();
    return exports ? (*exports)->getdf_di_joystick() : nullptr;
}

// Initialize optional GameExt before executable constructors; DirectInput forwarding and FC startup remain deferred.
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void *) {
    if (reason == DLL_PROCESS_ATTACH) {
        loader_module = instance;
        DisableThreadLibraryCalls(instance);
        load_optional_game_ext();
    }
    return TRUE;
}
