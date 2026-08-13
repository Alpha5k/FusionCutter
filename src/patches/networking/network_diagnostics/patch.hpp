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
    std::uint32_t maximum_file_size_mb{512};
};

[[nodiscard]] PatchDefinition definition();

} // namespace fusioncutter::patches::network_diagnostics
