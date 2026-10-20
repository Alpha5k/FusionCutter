#pragma once

#include "selection.hpp"
#include "plugins/plugin.hpp"
#include <span>
#include <string_view>
#include <vector>

namespace fc::runtime {

struct SelectionSession;

// Supplies a configured enabled value for a patch or group within one plugin.
struct ControlOverride {
    std::string_view local_id;
    bool enabled{};
};

// Identifies which declaration or configuration value determined a patch's enabled state.
enum class ControlSource {
    Default,
    Patch,
    Group
};

struct PatchControl {
    bool enabled{};
    ControlSource source{ControlSource::Default};
};

// Returns the reporting label for the declaration or setting that supplied an enabled state.
std::string_view control_source_name(ControlSource) noexcept;

// Returns effective controls in admitted patch order, matching override IDs ASCII case-insensitively.
// Configurable patches use their own controls; other patches inherit a configurable group's control or their own default.
std::vector<PatchControl> resolve_controls(const AdmittedPlugin &, std::span<const ControlOverride>);

// Connects required patches and groups, detects dependency cycles and updates waiting or failed consumers.
// Call after refreshing each patch's image selection; records and admitted declarations must remain valid.
void resolve_dependencies(SelectionSession &);

} // namespace fc::runtime
