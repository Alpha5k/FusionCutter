#pragma once

#include "installation.hpp"

#include <FusionCutter/Abi.h>

namespace fc::runtime {

// Distinguishes an inert poll, a transition that marks status dirty for the reporting owner, and an integrity failure.
enum class LateImageResult {
    Unchanged,
    Changed,
    Fatal,
};

// Test injection replaces only module detection; ownership, transitions, validation, and installation stay production.
using LateProbeFunction = targets::LateProbeResult (*)(void* context, const targets::RecognizedTarget& target,
                                                       FC_TargetImage image);

// Supplies the private probe seam and the same optional installer adapters used by lifecycle component tests.
struct LateImageServices {
    void* probe_context{};
    LateProbeFunction probe{};
    InstallationServices installation;
    CoreLogger logger;
    CoreLogger planning_logger;
};

// Processes each awaited image once in TargetImage order on the serialized thread that invokes Update callbacks.
[[nodiscard]] LateImageResult process_awaited_images(PatchRuntimeState& runtime, TraceSession& traces,
                                                     CrashReporter& crash, LateImageServices services = {});

} // namespace fc::runtime
