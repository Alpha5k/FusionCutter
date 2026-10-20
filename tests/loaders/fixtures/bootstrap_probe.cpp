#include <fc/plugin.hpp>

#include <Windows.h>

#include <array>
#include <filesystem>

namespace {

// Supplies the valid declaration when the copied DLL's basename selects no failure mode.
fc::Plugin build_plugin() {
    return {.id = "probe", .version = "1"};
}
} // namespace

// Returns a fixture declaration or failure result selected by this copied DLL's basename.
FC_PLUGIN_ENTRY
extern "C" FC_EXPORT std::uint32_t FC_CDECL FC_GetPlugin(std::uint32_t abi, const FC_Plugin **out, FC_ErrorSink sink) {
    if (!out) {
        return 0;
    }

    *out = nullptr;
    // Locate this export's own DLL so renamed copies select their fixture behavior independently.
    HMODULE module{};
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&FC_GetPlugin), &module);

    std::array<wchar_t, 32768> path{};
    GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    // Use the basename so a parent path cannot accidentally select another probe mode.
    const auto name = std::filesystem::path{path.data()}.stem().wstring();

    // Return a successful entry with a short descriptor to exercise runtime prefix validation.
    if (name == L"BadSize") {
        static const FC_Plugin declaration{.byte_size = 4};
        *out = &declaration;
        return 1;
    }
    // The loader requires exactly 1; another nonzero result must reject and unload this DLL.
    if (name == L"BadResult") {
        return 2;
    }
    return fc::detail::get_plugin<&build_plugin>(abi, out, sink);
}
