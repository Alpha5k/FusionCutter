#include "process_images.hpp"

#include "layouts/profiles.hpp"

#include <Windows.h>
#include <Psapi.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace fusioncutter::targets {
namespace {

[[nodiscard]] std::expected<RecognizedImage, ProcessImageError> recognize_module(HMODULE module, HostRole role) {
    if (module == nullptr) {
        return std::unexpected(ProcessImageError{"required module is not loaded", GetLastError()});
    }

    const auto process = GetCurrentProcess();
    MODULEINFO module_info{};
    if (!K32GetModuleInformation(process, module, &module_info, sizeof(module_info))) {
        return std::unexpected(ProcessImageError{"loaded module information is unavailable", GetLastError()});
    }
    if (module_info.lpBaseOfDll == nullptr || module_info.SizeOfImage == 0) {
        return std::unexpected(ProcessImageError{"loaded module information is invalid", ERROR_BAD_EXE_FORMAT});
    }

    std::array<char, MAX_PATH> basename{};
    const auto basename_size =
        K32GetModuleBaseNameA(process, module, basename.data(), static_cast<DWORD>(basename.size()));
    if (basename_size == 0 || basename_size >= basename.size()) {
        const auto error = basename_size == 0 ? GetLastError() : ERROR_INSUFFICIENT_BUFFER;
        return std::unexpected(ProcessImageError{"loaded module basename is unavailable", error});
    }

    const auto base = reinterpret_cast<std::uintptr_t>(module_info.lpBaseOfDll);
    const auto image = std::span{reinterpret_cast<const std::byte*>(module_info.lpBaseOfDll),
                                 static_cast<std::size_t>(module_info.SizeOfImage)};
    auto recognized = recognize_mapped_image(std::string_view{basename.data(), basename_size}, role, base, image,
                                             layouts::known_image_profiles());
    if (!recognized.has_value()) {
        return std::unexpected(ProcessImageError{recognized.error().detail, ERROR_BAD_EXE_FORMAT});
    }
    return *recognized;
}

} // namespace

std::expected<StartupImages, ProcessImageError> recognize_current_process_images(HostRole role) {
    auto executable = recognize_module(GetModuleHandleW(nullptr), role);
    if (!executable.has_value()) {
        return std::unexpected(executable.error());
    }

    if (executable->context.layout != TargetLayout::Aspyr) {
        return StartupImages{*executable, std::nullopt};
    }

    if (executable->context.image.identity != TargetImage::Bootstrap) {
        return std::unexpected(
            ProcessImageError{"Aspyr process executable is not the bootstrap", ERROR_BAD_EXE_FORMAT});
    }

    const auto game_module_name = role == HostRole::Client ? L"Battlefront2.original.dll" : L"Battlefront2.dll";
    auto game_module = recognize_module(GetModuleHandleW(game_module_name), role);
    if (!game_module.has_value()) {
        return std::unexpected(game_module.error());
    }

    if (game_module->context.layout != TargetLayout::Aspyr ||
        game_module->context.image.identity != TargetImage::Game) {
        return std::unexpected(ProcessImageError{"Aspyr game module has the wrong identity", ERROR_BAD_EXE_FORMAT});
    }

    return StartupImages{*executable, *game_module};
}

std::expected<std::optional<RecognizedImage>, ProcessImageError>
recognize_loaded_process_image(TargetLayout layout, HostRole role, TargetImage image) {
    if (layout != TargetLayout::GOGRetail || image != TargetImage::GalaxyPeer) {
        return std::unexpected(ProcessImageError{"late image is not supported for this target", ERROR_NOT_SUPPORTED});
    }

    const auto module = GetModuleHandleW(L"GalaxyPeer.dll");
    if (module == nullptr) {
        return std::optional<RecognizedImage>{};
    }

    auto recognized = recognize_module(module, role);
    if (!recognized.has_value()) {
        return std::unexpected(recognized.error());
    }
    if (recognized->context.layout != layout || recognized->context.image.identity != image) {
        return std::unexpected(ProcessImageError{"loaded late image has the wrong identity", ERROR_BAD_EXE_FORMAT});
    }
    return std::optional<RecognizedImage>{*recognized};
}

} // namespace fusioncutter::targets
