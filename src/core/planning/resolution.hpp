#pragma once

#include "planning_types.hpp"

#include "../core_logger.hpp"
#include "../targets/recognition.hpp"

namespace fc::planning {

// Borrows the immutable catalog, recognized images, and aligned raw configuration used by one selection closure.
struct ResolutionInput {
    const catalog::Catalog& catalog;
    const targets::RecognizedTarget& target;
    const config::ConfigurationSnapshot& configuration;
    // The optional capability reports selection decisions without coupling resolution to the reporting backend.
    CoreLogger logger;
};

// Resolves startup relationships without I/O and leaves Pending only selected patches whose images are ready.
[[nodiscard]] PatchWorkSet resolve_patches(const ResolutionInput& input);

// After every later validation or installation failure, removes consumers whose required provider became unavailable.
void prune_unavailable_consumers(PatchWorkSet& patches);

} // namespace fc::planning
