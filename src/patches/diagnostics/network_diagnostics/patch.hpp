#pragma once

#include <FusionCutter/patch.hpp>

#include <cstdint>

namespace fusioncutter::patches::network_diagnostics {

enum class CaptureMode {
    Standard = 0,
    Combat = 1,
    Full = 2,
};

struct NetworkDiagnosticsSettings {
    CaptureMode capture{CaptureMode::Standard};
};

[[nodiscard]] PatchDefinition definition();

} // namespace fusioncutter::patches::network_diagnostics
