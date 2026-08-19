#pragma once

#include <FusionCutter/patching.hpp>
#include <FusionCutter/target.hpp>

namespace fusioncutter::patches::network_pipeline {

struct ClientReceiveBoundary;

}

namespace fusioncutter::patches::network_diagnostics {

class NetworkDiagnostics;
enum class CaptureMode;

void build_client_plan(PatchPlan& plan, const TargetContext& target);
void build_server_plan(PatchPlan& plan, const TargetContext& target);
void build_client_codec_plan(PatchPlan& plan, const TargetContext& target, CaptureMode mode);
void build_client_combat_plan(PatchPlan& plan, const TargetContext& target);
void build_server_codec_plan(PatchPlan& plan, const TargetContext& target, CaptureMode mode);
void build_server_combat_plan(PatchPlan& plan, const TargetContext& target);

// Folds the shared client receive transaction into the active candidate observations.
void observe_client_receive(NetworkDiagnostics& diagnostics, const network_pipeline::ClientReceiveBoundary& boundary,
                            bool begin) noexcept;

void publish_observers(NetworkDiagnostics& diagnostics) noexcept;
void clear_observers(NetworkDiagnostics& diagnostics) noexcept;
[[nodiscard]] NetworkDiagnostics* active_diagnostics() noexcept;

} // namespace fusioncutter::patches::network_diagnostics
