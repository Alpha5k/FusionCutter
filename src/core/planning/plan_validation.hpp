#pragma once

#include "planning_types.hpp"

#include "../core_logger.hpp"
#include "../targets/recognition.hpp"

namespace fc::planning {

// Runs shared Create and Plan callbacks plus global validation for every record in the Pending state.
[[nodiscard]] InstallationPlan build_installation_plan(const targets::RecognizedTarget& target, PatchWorkSet& patches,
                                                       ValidationBaseline baseline = {}, CoreLogger logger = {});

} // namespace fc::planning
