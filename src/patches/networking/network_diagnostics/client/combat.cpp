#include "../observers.hpp"

#include "layout.hpp"
#include "../combat.hpp"

namespace fusioncutter::patches::network_diagnostics {

void build_client_combat_plan(PatchPlan& plan, const TargetContext& target) {
    build_combat_plan(plan, client::layout_for(target.layout).combat);
}

} // namespace fusioncutter::patches::network_diagnostics
