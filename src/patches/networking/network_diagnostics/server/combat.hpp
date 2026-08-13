#pragma once

#include <cstdint>

namespace fusioncutter::patches::network_diagnostics {

class NetworkDiagnostics;

void write_authoritative_poses(NetworkDiagnostics& diagnostics, std::int32_t turn) noexcept;

} // namespace fusioncutter::patches::network_diagnostics
