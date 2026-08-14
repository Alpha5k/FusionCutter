#pragma once

#include <FusionCutter/patch.hpp>

#include <cstdint>

namespace fusioncutter::patches::hero_diagnostics {

enum class CaptureMode : std::uint8_t {
    Standard = 1,
    Combat = 2,
    Full = 3,
};

struct HeroDiagnosticsSettings {
    CaptureMode capture{CaptureMode::Standard};
};

PatchDefinition definition();

} // namespace fusioncutter::patches::hero_diagnostics
