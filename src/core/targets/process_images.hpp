#pragma once

#include "recognition.hpp"

#include <cstdint>
#include <expected>
#include <optional>
#include <string_view>

namespace fusioncutter::targets {

struct StartupImages {
    RecognizedImage executable;
    std::optional<RecognizedImage> game_module;
};

struct ProcessImageError {
    std::string_view detail;
    std::uint32_t windows_error;
};

[[nodiscard]] std::expected<StartupImages, ProcessImageError> recognize_current_process_images(HostRole role);

[[nodiscard]] std::expected<std::optional<RecognizedImage>, ProcessImageError>
recognize_loaded_process_image(TargetLayout layout, HostRole role, TargetImage image);

} // namespace fusioncutter::targets
