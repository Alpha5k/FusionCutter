#include "plugin.hpp"

#include <fc/base.hpp>

#include <Windows.h>

#include <algorithm>

namespace fc::runtime {

namespace {

// Collects errors reported by FC_GetPlugin.
struct PluginErrorCapture {
    std::string message;
    std::uint32_t windows_error{};

    // Copies message because the non-owning view's data is valid only during the callback.
    static void FC_CDECL write(void *context, FC_Text message, std::uint32_t windows_error) {
        auto &self = *static_cast<PluginErrorCapture *>(context);
        if (message.size != 0 && message.data != nullptr) {
            self.message.assign(message.data, message.size);
        }
        self.windows_error = windows_error;
    }
};

} // namespace

AdmissionResult load_plugins(std::span<const std::filesystem::path> paths, std::span<const DefinitionCandidate> embedded) {
    AdmissionResult result;
    std::vector<DefinitionCandidate> definitions{embedded.begin(), embedded.end()};

    // Tracks each loader reference until validation decides whether the DLL must stay loaded.
    struct LoadedModule {
        const FC_Plugin *definition;
        HMODULE module;
    };

    std::vector<LoadedModule> loaded_modules;
    definitions.reserve(paths.size());

    // Check every path independently and retain each loading or entry point failure.
    for (const auto &path : paths) {
        const auto absolute = std::filesystem::absolute(path);
        const auto owner = path_text(absolute);

        // Windows resolves imports beside this DLL or in System32 with these search flags.
        auto module = LoadLibraryExW(absolute.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (module == nullptr) {
            const auto error = GetLastError();
            std::string message =
                error == ERROR_BAD_EXE_FORMAT
                    ? "could not load plugin: Windows rejected the DLL format; its architecture may differ from the process"
                    : "could not load plugin";
            result.diagnostics.push_back({owner, std::move(message), error});
            continue;
        }

        // Resolve the C entry by its undecorated name on either process architecture.
        const auto entry = reinterpret_cast<FC_GetPluginFn>(GetProcAddress(module, "FC_GetPlugin"));
        if (entry == nullptr) {
            const auto error = GetLastError();
            result.diagnostics.push_back({owner, "plugin must export the undecorated FC_GetPlugin entry point", error});
            FreeLibrary(module);
            continue;
        }

        // Request this ABI version; success requires both the exact result value and a declaration.
        PluginErrorCapture capture;
        const FC_Plugin *definition{};
        if (entry(FC_ABI_VERSION, &definition, {&capture, &PluginErrorCapture::write}) != 1 || definition == nullptr) {
            result.diagnostics.push_back(
                {owner, capture.message.empty() ? "FC_GetPlugin failed without supplying an error message" : capture.message,
                 capture.windows_error});
            FreeLibrary(module);
            continue;
        }

        definitions.push_back({owner, definition});
        loaded_modules.push_back({definition, module});
    }

    // Keep all DLLs loaded while validating the declarations and nested views stored in them.
    auto admitted = admit_definitions(definitions);
    result.admitted = std::move(admitted.admitted);
    result.diagnostics.insert(result.diagnostics.end(), std::make_move_iterator(admitted.diagnostics.begin()),
                              std::make_move_iterator(admitted.diagnostics.end()));

    // The original loader reference keeps accepted declarations valid until process exit.
    for (const auto &loaded_module : loaded_modules) {
        const auto accepted = std::ranges::any_of(result.admitted, [&](const auto &plugin) {
            return plugin.definition == loaded_module.definition;
        });
        if (!accepted) {
            FreeLibrary(loaded_module.module);
        }
    }

    return result;
}

std::expected<std::vector<std::filesystem::path>, Diagnostic> discover_plugins(const std::filesystem::path &directory) {
    // An absent plugin directory is an empty collection; other filesystem failures become diagnostics.
    std::error_code error;
    std::filesystem::directory_iterator iterator{directory, error}, end;
    if (error == std::errc::no_such_file_or_directory) {
        return std::vector<std::filesystem::path>{};
    }

    // Discover regular DLL files in this directory using ASCII case-insensitive extensions.
    std::vector<std::filesystem::path> paths;
    for (; !error && iterator != end; iterator.increment(error)) {
        if (iterator->is_regular_file(error) && fc::detail::equal_id(path_text(iterator->path().extension()), ".dll")) {
            paths.push_back(iterator->path());
        }
    }

    if (error) {
        return std::unexpected(
            Diagnostic{path_text(directory), "Could not read the plugin directory", static_cast<std::uint32_t>(error.value())});
    }

    // Sort case-insensitively, breaking ties by spelling so directory enumeration order cannot affect admission.
    std::ranges::sort(paths, [](const auto &a, const auto &b) {
        const auto first_name = path_text(a.filename()), second_name = path_text(b.filename());
        const auto first = fc::detail::folded_id(first_name), second = fc::detail::folded_id(second_name);
        return first == second ? first_name < second_name : first < second;
    });
    return paths;
}

} // namespace fc::runtime
