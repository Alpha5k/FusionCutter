#pragma once

#include "../../../pipelines/soldier_state/pipeline.hpp"

namespace fusioncutter::patches::network_diagnostics {

class NetworkDiagnostics;

[[nodiscard]] soldier_state_pipeline::ObserverCallbacks
make_soldier_state_observer(NetworkDiagnostics& diagnostics) noexcept;

void begin_authoritative_update() noexcept;
void finish_authoritative_update() noexcept;
void begin_presentation_frame() noexcept;

} // namespace fusioncutter::patches::network_diagnostics
