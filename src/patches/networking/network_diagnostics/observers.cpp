#include "observers.hpp"

#include "network_diagnostics.hpp"

#include <FusionCutter/patch.hpp>

namespace fusioncutter::patches::network_diagnostics {
namespace {

PatchInstanceSlot<NetworkDiagnostics> gActive;

} // namespace

void publish_observers(NetworkDiagnostics& diagnostics) noexcept {
    gActive.publish(diagnostics);
}

void clear_observers(NetworkDiagnostics& diagnostics) noexcept {
    gActive.clear(diagnostics);
}

NetworkDiagnostics* active_diagnostics() noexcept {
    return gActive.read();
}

} // namespace fusioncutter::patches::network_diagnostics
