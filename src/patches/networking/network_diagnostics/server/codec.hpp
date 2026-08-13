#pragma once

#include <cstdint>

namespace fusioncutter::patches::network_diagnostics {

void begin_destination_update(std::int32_t destination) noexcept;
void finish_destination_update() noexcept;

} // namespace fusioncutter::patches::network_diagnostics
