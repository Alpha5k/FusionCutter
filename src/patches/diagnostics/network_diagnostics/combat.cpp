#include "combat.hpp"

#include "network_diagnostics.hpp"
#include "observers.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>

namespace fusioncutter::patches::network_diagnostics {
namespace {

using WeaponFire = bool(__fastcall*)(void*, void*) noexcept;
using BulletBuild = void*(__fastcall*)(void*, void*, const void*) noexcept;
using BulletUpdate = bool(__fastcall*)(void*, void*, float) noexcept;
using ApplyDamage = bool(__fastcall*)(void*, void*, const void*, std::uint32_t) noexcept;

OriginalFunction<WeaponFire> gWeaponFire;
OriginalFunction<BulletBuild> gBulletBuild;
OriginalFunction<BulletUpdate> gBulletUpdate;
OriginalFunction<ApplyDamage> gApplyDamage;
thread_local trace::WeaponFireRecord* gActiveFire{};
thread_local void* gActiveProjectile{};
thread_local std::uint32_t gActiveProjectileId{};
thread_local trace::ProjectileSimulationRecord gProjectileSimulation{};
thread_local std::uint32_t gRayDepth{};

enum class ProjectileSlotState : std::uint8_t {
    Empty,
    Active,
    Retired,
};

struct ProjectileIdentity {
    const void* projectile{};
    std::uint32_t id{};
    ProjectileSlotState state{};
};

constexpr std::size_t kProjectileIdentityCount = 4096;
constexpr std::size_t kProjectileIdentityMask = kProjectileIdentityCount - 1;
static_assert((kProjectileIdentityCount & kProjectileIdentityMask) == 0);

std::array<ProjectileIdentity, kProjectileIdentityCount> gProjectileIdentities{};
std::atomic_uint32_t gNextProjectileId{1};

[[nodiscard]] std::uint32_t pointer_id(const void* pointer) noexcept {
    return static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(pointer));
}

[[nodiscard]] std::size_t projectile_slot(const void* projectile) noexcept {
    return (reinterpret_cast<std::uintptr_t>(projectile) >> 4U) & kProjectileIdentityMask;
}

// Gives each constructed projectile a stable trace identity until its simulation ends.
[[nodiscard]] std::uint32_t assign_projectile_id(const void* projectile) noexcept {
    if (projectile == nullptr) {
        return 0;
    }

    ProjectileIdentity* reusable{};
    const auto start = projectile_slot(projectile);
    for (std::size_t offset = 0; offset < gProjectileIdentities.size(); ++offset) {
        auto& identity = gProjectileIdentities[(start + offset) & kProjectileIdentityMask];
        if (identity.state == ProjectileSlotState::Active && identity.projectile == projectile) {
            reusable = &identity;
            break;
        }
        if (reusable == nullptr && identity.state == ProjectileSlotState::Retired) {
            reusable = &identity;
        }
        if (identity.state == ProjectileSlotState::Empty) {
            reusable = reusable == nullptr ? &identity : reusable;
            break;
        }
    }
    if (reusable == nullptr) {
        reusable = &gProjectileIdentities[start];
    }

    auto id = gNextProjectileId.fetch_add(1, std::memory_order_relaxed);
    if (id == 0) {
        id = gNextProjectileId.fetch_add(1, std::memory_order_relaxed);
    }
    *reusable = {.projectile = projectile, .id = id, .state = ProjectileSlotState::Active};
    return id;
}

[[nodiscard]] std::uint32_t find_projectile_id(const void* projectile) noexcept {
    if (projectile == nullptr) {
        return 0;
    }

    const auto start = projectile_slot(projectile);
    for (std::size_t offset = 0; offset < gProjectileIdentities.size(); ++offset) {
        const auto& identity = gProjectileIdentities[(start + offset) & kProjectileIdentityMask];
        if (identity.state == ProjectileSlotState::Empty) {
            break;
        }
        if (identity.state == ProjectileSlotState::Active && identity.projectile == projectile) {
            return identity.id;
        }
    }
    return 0;
}

void retire_projectile_id(const void* projectile) noexcept {
    if (projectile == nullptr) {
        return;
    }

    const auto start = projectile_slot(projectile);
    for (std::size_t offset = 0; offset < gProjectileIdentities.size(); ++offset) {
        auto& identity = gProjectileIdentities[(start + offset) & kProjectileIdentityMask];
        if (identity.state == ProjectileSlotState::Empty) {
            return;
        }
        if (identity.state == ProjectileSlotState::Active && identity.projectile == projectile) {
            identity.state = ProjectileSlotState::Retired;
            return;
        }
    }
}

template <typename Value> [[nodiscard]] Value read_field(const void* object, std::size_t offset) noexcept {
    return object == nullptr ? Value{}
                             : *reinterpret_cast<const Value*>(static_cast<const std::byte*>(object) + offset);
}

template <typename Value, std::size_t Size>
void copy_values(std::array<Value, Size>& output, const void* input) noexcept {
    if (input != nullptr) {
        std::memcpy(output.data(), input, sizeof(output));
    }
}

// Captures the firing transform already produced by the game's Aimer call.
void observe_fire_matrix(MidHookContext& context) noexcept {
    if (gActiveFire != nullptr && context.esp != 0) {
        copy_values(gActiveFire->matrix, reinterpret_cast<const std::byte*>(context.esp) + 0x90);
    }
}

// Records a cannon fire attempt and the exact transform used by the game.
bool __fastcall observe_weapon_fire(void* weapon, void*) noexcept {
    const auto start = trace::read_stamp();
    trace::WeaponFireRecord record{
        .weapon = pointer_id(weapon),
        .owner = pointer_id(read_field<void*>(weapon, 0x6C)),
        .aimer = pointer_id(read_field<void*>(weapon, 0x70)),
    };
    auto* previous = gActiveFire;
    gActiveFire = &record;
    const auto original = gWeaponFire.get();
    const auto accepted = original != nullptr && original(weapon, nullptr);
    gActiveFire = previous;
    record.accepted = accepted;
    record.duration = trace::read_stamp().timestamp - start.timestamp;
    if (auto* diagnostics = active_diagnostics(); diagnostics != nullptr) {
        diagnostics->recorder().submit(trace::RecordKind::WeaponFire, trace::payload_bytes(record), record.weapon,
                                       accepted ? trace::RecordFlags::Accepted | trace::RecordFlags::Combat
                                                : trace::RecordFlags::Rejected | trace::RecordFlags::Combat);
    }
    return accepted;
}

// Records the projectile returned by OrdnanceBullet::Build.
void* __fastcall observe_bullet_build(void* projectile_class, void*, const void* descriptor) noexcept {
    const auto start = trace::read_stamp();
    const auto original = gBulletBuild.get();
    auto* projectile = original == nullptr ? nullptr : original(projectile_class, nullptr, descriptor);
    const auto projectile_id = assign_projectile_id(projectile);
    if (auto* diagnostics = active_diagnostics(); diagnostics != nullptr) {
        trace::ProjectileBuildRecord record{
            .projectile = pointer_id(projectile),
            .projectile_id = projectile_id,
            .projectile_class = pointer_id(projectile_class),
            .descriptor = pointer_id(descriptor),
            .accepted = projectile != nullptr,
            .lifetime = read_field<float>(projectile, 0x3C),
            .duration = trace::read_stamp().timestamp - start.timestamp,
        };
        copy_values(record.position, projectile == nullptr ? nullptr : static_cast<std::byte*>(projectile) + 0x48);
        copy_values(record.velocity, projectile == nullptr ? nullptr : static_cast<std::byte*>(projectile) + 0xFC);
        diagnostics->recorder().submit(trace::RecordKind::ProjectileBuild, trace::payload_bytes(record), projectile_id,
                                       projectile != nullptr
                                           ? trace::RecordFlags::Accepted | trace::RecordFlags::Combat
                                           : trace::RecordFlags::Rejected | trace::RecordFlags::Combat);
    }
    return projectile;
}

// Scopes the generic collision observers to one bullet simulation step.
bool __fastcall observe_bullet_update(void* projectile, void*, float delta) noexcept {
    const auto start = trace::read_stamp();
    const auto previous = gActiveProjectile;
    const auto previous_id = gActiveProjectileId;
    const auto previous_simulation = gProjectileSimulation;
    const auto previous_ray_depth = gRayDepth;
    gActiveProjectile = projectile;
    gActiveProjectileId = find_projectile_id(projectile);
    gProjectileSimulation = {.projectile = pointer_id(projectile), .delta = delta};
    gRayDepth = 0;
    const auto original = gBulletUpdate.get();
    const auto result = original != nullptr && original(projectile, nullptr, delta);
    gProjectileSimulation.duration = trace::read_stamp().timestamp - start.timestamp;
    copy_values(gProjectileSimulation.position,
                projectile == nullptr ? nullptr : static_cast<std::byte*>(projectile) + 0x48);
    copy_values(gProjectileSimulation.velocity,
                projectile == nullptr ? nullptr : static_cast<std::byte*>(projectile) + 0xFC);
    if (auto* diagnostics = active_diagnostics(); diagnostics != nullptr) {
        std::uint16_t flags = trace::RecordFlags::Combat;
        if (!result) {
            flags |= trace::RecordFlags::End;
        }
        diagnostics->recorder().submit(trace::RecordKind::ProjectileSimulation,
                                       trace::payload_bytes(gProjectileSimulation), gActiveProjectileId, flags);
    }
    if (!result) {
        retire_projectile_id(projectile);
    }
    gActiveProjectile = previous;
    gActiveProjectileId = previous_id;
    gProjectileSimulation = previous_simulation;
    gRayDepth = previous_ray_depth;
    return result;
}

// Retains one complete outer collision query while counting nested ray work.
void observe_projectile_ray(MidHookContext& context) noexcept {
    if (gActiveProjectile == nullptr || context.ecx == 0 || context.edx == 0 || context.esp == 0) {
        return;
    }
    const auto* stack = reinterpret_cast<const std::byte*>(context.esp);
    ++gProjectileSimulation.rays;
    if (gRayDepth++ != 0) {
        return;
    }
    gProjectileSimulation.ray_filter = pointer_id(read_native_field<void*>(stack, 0x08));
    gProjectileSimulation.ray_filter_secondary = pointer_id(read_native_field<void*>(stack, 0x0C));
    gProjectileSimulation.ray_filter_word = read_native_field<std::uint32_t>(stack, 0x10);
    gProjectileSimulation.ray_flags = (read_native_field<std::uint32_t>(stack, 0x14) & 0x00FF'FFFFU) |
                                      ((read_native_field<std::uint32_t>(stack, 0x18) & 0xFFU) << 24U);
    gProjectileSimulation.ray_length = context.xmm2.f32[0];
    copy_values(gProjectileSimulation.origin, reinterpret_cast<const void*>(context.ecx));
    copy_values(gProjectileSimulation.direction, reinterpret_cast<const void*>(context.edx));
}

// Completes the active collision query with its result and hit object.
void observe_projectile_ray_result(MidHookContext& context) noexcept {
    if (gActiveProjectile == nullptr || gRayDepth == 0 || --gRayDepth != 0) {
        return;
    }
    gProjectileSimulation.ray_target = pointer_id(reinterpret_cast<const void*>(context.eax));
    gProjectileSimulation.ray_result = context.xmm0.f32[0];
}

// Records the health and shield mutation produced by native damage application.
bool __fastcall observe_apply_damage(void* damageable, void*, const void* descriptor, std::uint32_t flags) noexcept {
    const auto start = trace::read_stamp();
    trace::DamageRecord record{
        .damageable = pointer_id(damageable),
        .descriptor = pointer_id(descriptor),
        .projectile = pointer_id(gActiveProjectile),
        .projectile_id = gActiveProjectileId,
        .flags = flags,
        .health_before = read_field<float>(damageable, 0x04),
        .shield_before = read_field<float>(damageable, 0x10),
    };
    const auto original = gApplyDamage.get();
    const auto alive = original != nullptr && original(damageable, nullptr, descriptor, flags);
    record.health_after = read_field<float>(damageable, 0x04);
    record.shield_after = read_field<float>(damageable, 0x10);
    record.alive_after = alive;
    record.duration = trace::read_stamp().timestamp - start.timestamp;
    if (auto* diagnostics = active_diagnostics(); diagnostics != nullptr) {
        diagnostics->recorder().submit(trace::RecordKind::Damage, trace::payload_bytes(record), record.damageable,
                                       trace::RecordFlags::Combat);
    }
    return alive;
}

} // namespace

void build_combat_plan(PatchPlan& plan, const CombatLayout& layout) {
    gWeaponFire = plan.inline_hook_with_original("Observe cannon fire", layout.weapon_fire.rva,
                                                 layout.weapon_fire.pattern(), &observe_weapon_fire);
    plan.mid_hook("Capture cannon firing transform", layout.fire_matrix.rva, layout.fire_matrix.pattern(),
                  &observe_fire_matrix);
    gBulletBuild = plan.inline_hook_with_original("Observe projectile construction", layout.bullet_build.rva,
                                                  layout.bullet_build.pattern(), &observe_bullet_build);
    gBulletUpdate = plan.inline_hook_with_original("Scope projectile collision rays", layout.bullet_update.rva,
                                                   layout.bullet_update.pattern(), &observe_bullet_update);
    plan.mid_hook("Observe projectile collision rays", layout.ray_hit.rva, layout.ray_hit.pattern(),
                  &observe_projectile_ray);
    plan.mid_hook("Observe projectile collision results", layout.ray_return.rva, layout.ray_return.pattern(),
                  &observe_projectile_ray_result);
    gApplyDamage = plan.inline_hook_with_original("Observe damage application", layout.apply_damage.rva,
                                                  layout.apply_damage.pattern(), &observe_apply_damage);
}

} // namespace fusioncutter::patches::network_diagnostics
