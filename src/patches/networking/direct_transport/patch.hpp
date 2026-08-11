#pragma once

#include <FusionCutter/patch.hpp>

#include "shared/protocol.hpp"

namespace fusioncutter::patches::direct_transport {

struct DirectTransportSettings {
    Policy policy;
};

[[nodiscard]] PatchDefinition definition();

} // namespace fusioncutter::patches::direct_transport
