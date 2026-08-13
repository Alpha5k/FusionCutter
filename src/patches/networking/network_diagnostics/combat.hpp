#pragma once

#include <FusionCutter/patching.hpp>

namespace fusioncutter::patches::network_diagnostics {

struct CombatLayout {
    NativeSite<8> weapon_fire;
    NativeSite<11> fire_matrix;
    NativeSite<5> bullet_build;
    NativeSite<8> bullet_update;
    NativeSite<10> ray_hit;
    NativeSite<5> ray_return;
    NativeSite<7> apply_damage;
};

// Installs the fire, projectile, collision, and damage observers shared by both roles.
void build_combat_plan(PatchPlan& plan, const CombatLayout& layout);

} // namespace fusioncutter::patches::network_diagnostics
