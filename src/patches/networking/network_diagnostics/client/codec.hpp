#pragma once

namespace fusioncutter::patches::network_diagnostics {

void begin_authoritative_update() noexcept;
void finish_authoritative_update() noexcept;
void begin_presentation_frame() noexcept;

} // namespace fusioncutter::patches::network_diagnostics
