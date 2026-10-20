#pragma once

#include <fc/abi.h>
#include <Windows.h>
#include <array>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>

namespace fc::tests::loaders {

// Looks up an export using F, which must match its signature; returns null if lookup fails.
// Keep the module loaded while calling the returned function.
template <class F> F resolve(HMODULE module, const char *name) {
    return reinterpret_cast<F>(GetProcAddress(module, name));
}

// Copies a fixture over the destination; returns false if the file operation fails.
inline bool copy_fixture(const std::filesystem::path &source, const std::filesystem::path &destination) {
    std::error_code error;
    return std::filesystem::copy_file(source, destination, std::filesystem::copy_options::overwrite_existing, error) && !error;
}

// Returns the directory supplied by the parent harness; throws if its environment value is missing or too long.
inline std::filesystem::path stage() {
    std::array<wchar_t, 32768> supplied{};
    const auto length = GetEnvironmentVariableW(L"FC_TEST_LOADER_DIRECTORY", supplied.data(), static_cast<DWORD>(supplied.size()));
    if (length == 0 || length >= supplied.size()) {
        throw std::runtime_error("The loader child requires its harness staging directory");
    }
    return supplied.data();
}

// Places the client loader and the scenario's runtime fixture together in the temporary directory.
// Selects the stub's startup result; returns zero on success or a child failure code.
inline int prepare_client(const std::filesystem::path &directory, std::string_view scenario,
                          const std::filesystem::path &loader_path = FC_CLIENT_LOADER_PATH,
                          const std::filesystem::path &loader_name = FC_CLIENT_LOADER_NAME) {
    if (!copy_fixture(loader_path, directory / loader_name)) {
        return 10;
    }
    if (scenario != "missing-runtime" &&
        !copy_fixture(scenario == "missing-export" ? FC_NO_EXPORT_PATH : FC_CORE_STUB_PATH, directory / L"FusionCutter.dll")) {
        return 11;
    }

    SetEnvironmentVariableA("FC_TEST_CORE_RESULT", scenario == "failed" ? "failed" : "completed");
    return 0;
}

// Checks that the loader called FC_Start exactly once with the current ABI and client mode.
inline bool startup_captured() {
    const auto core = GetModuleHandleW(L"FusionCutter.dll");
    if (!core) {
        return false;
    }

    const auto count = resolve<std::uint32_t (*)()>(core, "FC_Test_StartCount");
    const auto capture = resolve<BOOL (*)(FC_StartOptions *, std::uint32_t *)>(core, "FC_Test_CopyStartup");
    FC_StartOptions options{};
    std::uint32_t abi{};
    return count && capture && count() == 1 && capture(&options, &abi) && abi == FC_ABI_VERSION && options.byte_size == sizeof(options) &&
           options.dedicated == 0;
}

// Owns the startup error text and code for inspection after FC_Start returns.
struct StartError {
    std::string message;
    std::uint32_t code{};

    // context must point to a live StartError; copies the borrowed text during the synchronous callback.
    static void FC_CDECL receive(void *context, FC_Text text, std::uint32_t code) {
        *static_cast<StartError *>(context) = {std::string{text.data, text.size}, code};
    }
};

// Runs the architecture's client-loader scenario, retaining loaded DLLs until the child exits.
// Returns zero on success or a fixture failure code; fatal loader failures terminate the child.
int client(std::string_view scenario);
// Runs an embedded dedicated-runtime scenario, retaining loaded DLLs until the child exits.
// Returns zero on success or a fixture failure code; duplicate runtime startup terminates the child.
int combined(std::string_view scenario);
// Checks that invalid ABI and options requests fail before production runtime initialization.
int runtime_boundary();

} // namespace fc::tests::loaders
