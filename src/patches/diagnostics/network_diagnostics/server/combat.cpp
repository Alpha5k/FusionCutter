#include "../observers.hpp"

#include "combat.hpp"
#include "layout.hpp"
#include "../combat.hpp"
#include "../network_diagnostics.hpp"

#include <array>
#include <cstdint>
#include <cstring>

namespace fusioncutter::patches::network_diagnostics {
namespace {

using CreateOrdnanceEvent = void(__cdecl*)(const void*, const void*) noexcept;
using PlayerControllerUpdate = void(__fastcall*)(void*, void*, float) noexcept;

OriginalFunction<CreateOrdnanceEvent> gCreateOrdnanceEvent;
OriginalFunction<PlayerControllerUpdate> gPlayerControllerUpdate;
ImageContext gImage{};
std::uint32_t gEventHeadRva{};
std::uint32_t gSoldierControllableVtableRva{};
thread_local std::array<void*, 64> gSoldiers{};

[[nodiscard]] std::uint32_t pointer_id(const void* pointer) noexcept {
    return static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(pointer));
}

template <typename Value> [[nodiscard]] Value read_field(const void* object, std::size_t offset) noexcept {
    return object == nullptr ? Value{}
                             : *reinterpret_cast<const Value*>(static_cast<const std::byte*>(object) + offset);
}

template <typename Value> [[nodiscard]] Value read_global(std::uint32_t rva, Value fallback = {}) noexcept {
    const auto* value = gImage.read_at_rva<Value>(rva);
    return value == nullptr ? fallback : *value;
}

template <typename Value, std::size_t Size>
void copy_values(std::array<Value, Size>& output, const void* input) noexcept {
    if (input != nullptr) {
        std::memcpy(output.data(), input, sizeof(output));
    }
}

[[nodiscard]] bool is_soldier(const void* controllable) noexcept {
    const auto vtable = read_field<const void*>(controllable, 0);
    return vtable != nullptr && reinterpret_cast<std::uintptr_t>(vtable) == gImage.base + gSoldierControllableVtableRva;
}

// Records creation of the one-shot ordnance event that remote clients consume.
void __cdecl observe_create_ordnance_event(const void* projectile_class, const void* descriptor) noexcept {
    const auto before = read_global<std::uint32_t>(gEventHeadRva);
    if (const auto original = gCreateOrdnanceEvent.get(); original != nullptr) {
        original(projectile_class, descriptor);
    }
    if (auto* diagnostics = active_diagnostics(); diagnostics != nullptr) {
        trace::OrdnanceEventRecord record{
            .projectile_class = pointer_id(projectile_class),
            .descriptor = pointer_id(descriptor),
            .head_before = before,
            .head_after = read_global<std::uint32_t>(gEventHeadRva),
        };
        copy_values(record.origin, descriptor);
        copy_values(record.direction,
                    descriptor == nullptr ? nullptr : static_cast<const std::byte*>(descriptor) + 0xC);
        diagnostics->recorder().submit(trace::RecordKind::OrdnanceEvent, trace::payload_bytes(record),
                                       record.descriptor, trace::RecordFlags::Combat | trace::RecordFlags::End);
    }
}

// Records the controller state consumed by one Soldier update and retains that Soldier for the turn snapshot.
void __fastcall observe_player_controller(void* controller, void*, float delta) noexcept {
    const auto start = trace::read_stamp();
    auto* controllable = read_field<void*>(controller, 0x04);
    auto* input = read_field<void*>(controller, 0x88);
    trace::ControllerStateRecord record{
        .controller = pointer_id(controller),
        .controllable = pointer_id(controllable),
        .player = read_field<std::int32_t>(controllable, 0xD4),
        .delta = delta,
        .buttons = read_field<std::uint32_t>(input, 0x10),
        .primary_switch_before = read_field<std::uint32_t>(controllable, 0x78),
        .secondary_switch_before = read_field<std::uint32_t>(controllable, 0x7C),
    };
    copy_values(record.axes, input);
    if (const auto original = gPlayerControllerUpdate.get(); original != nullptr) {
        original(controller, nullptr, delta);
    }
    record.primary_switch_after = read_field<std::uint32_t>(controllable, 0x78);
    record.secondary_switch_after = read_field<std::uint32_t>(controllable, 0x7C);
    record.duration = trace::read_stamp().timestamp - start.timestamp;
    if (record.player >= 0 && record.player < static_cast<std::int32_t>(gSoldiers.size()) && is_soldier(controllable)) {
        gSoldiers[record.player] = controllable;
    }
    if (auto* diagnostics = active_diagnostics(); diagnostics != nullptr) {
        diagnostics->recorder().submit(trace::RecordKind::ControllerState, trace::payload_bytes(record),
                                       record.controllable, trace::RecordFlags::Combat);
    }
}

} // namespace

void write_authoritative_poses(NetworkDiagnostics& diagnostics, std::int32_t turn) noexcept {
    trace::ServerPoseBatch batch{.turn = turn};
    for (std::int32_t player = 0; player < static_cast<std::int32_t>(gSoldiers.size()); ++player) {
        auto* soldier = gSoldiers[player];
        if (soldier == nullptr) {
            continue;
        }
        auto& pose = batch.soldiers[batch.count++];
        pose.player = player;
        // PlayerController owns the Controllable subobject at EntitySoldier +0x240.
        const auto* entity = static_cast<const std::byte*>(soldier) - 0x240;
        std::memcpy(pose.position.data(), entity + 0x120, sizeof(pose.position));
        std::memcpy(pose.velocity.data(), entity + 0x4DC, sizeof(pose.velocity));
        if (batch.count == batch.soldiers.size()) {
            diagnostics.recorder().submit(trace::RecordKind::ServerSoldierPose, trace::payload_bytes(batch),
                                          static_cast<std::uint32_t>(turn), trace::RecordFlags::Combat);
            batch.count = 0;
        }
    }
    if (batch.count != 0) {
        diagnostics.recorder().submit(trace::RecordKind::ServerSoldierPose, trace::payload_bytes(batch),
                                      static_cast<std::uint32_t>(turn), trace::RecordFlags::Combat);
    }
    gSoldiers.fill(nullptr);
}

void build_server_combat_plan(PatchPlan& plan, const TargetContext& target) {
    const auto& layout = server::kGogLayout;
    gImage = target.image;
    gEventHeadRva = layout.event_head_rva;
    gSoldierControllableVtableRva = layout.soldier_controllable_vtable_rva;
    gCreateOrdnanceEvent =
        plan.inline_hook_with_original("Observe ordnance events", layout.create_ordnance_event.rva,
                                       layout.create_ordnance_event.pattern(), &observe_create_ordnance_event);
    gPlayerControllerUpdate =
        plan.inline_hook_with_original("Observe authoritative controller state", layout.player_controller_update.rva,
                                       layout.player_controller_update.pattern(), &observe_player_controller);
    build_combat_plan(plan, layout.combat);
}

} // namespace fusioncutter::patches::network_diagnostics
