#pragma once

#include "../recognition.hpp"

#include <span>

namespace fusioncutter::targets::layouts {

[[nodiscard]] std::span<const ImageProfile> known_image_profiles() noexcept;

} // namespace fusioncutter::targets::layouts
