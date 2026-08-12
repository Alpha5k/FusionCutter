#include "door_corpse_fix.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <span>

namespace fusioncutter::patches::door_corpse_fix {
namespace {

constexpr std::uint32_t kDoorQueryCallRva = 0x00098A41;
constexpr std::uint32_t kSoldierRttiGetterRva = 0x000DE050;
constexpr std::size_t kDamageableFlagsOffset = 0x1FC;
constexpr std::uint32_t kAliveFlag = 0x08;

constexpr auto kSteamQueryCall = byte_array<0xE8, 0xDA, 0x6D, 0x13, 0x00>();
constexpr auto kGogQueryCall = byte_array<0xE8, 0x5A, 0x7D, 0x13, 0x00>();
constexpr auto kQueryCallCleanup = byte_array<0x83, 0xC4, 0x18>();
constexpr auto kRttiGetter = byte_array<0xA1, 0x00, 0x00, 0x00, 0x00, 0xC3>();
constexpr auto kRttiGetterMask = byte_array<0xFF, 0x00, 0x00, 0x00, 0x00, 0xFF>();

[[nodiscard]] std::span<const std::byte> query_call(TargetLayout layout) noexcept {
    switch (layout) {
    case TargetLayout::SteamRetail:
        return kSteamQueryCall;
    case TargetLayout::GOGRetail:
        return kGogQueryCall;
    case TargetLayout::Aspyr:
    case TargetLayout::ModTools:
        std::unreachable();
    }
    std::unreachable();
}

} // namespace

DoorCorpseFix::DoorCorpseFix(const TargetContext& target) noexcept
    : layout_(target.layout),
      soldier_rtti_getter_(reinterpret_cast<const void*>(target.image.address_at_rva(kSoldierRttiGetterRva))) {}

void DoorCorpseFix::build_plan(PatchPlan& plan) {
    const auto call = query_call(layout_);
    plan.require_bytes("Verify door query call frame", kDoorQueryCallRva + call.size(),
                       BytePattern::exact(kQueryCallCleanup));
    plan.require_bytes("Verify EntitySoldier RTTI getter", kSoldierRttiGetterRva,
                       BytePattern::masked(kRttiGetter, kRttiGetterMask));
    // Redirect only EntityDoor's range query so other callers retain the unfiltered game behavior.
    original_query_ = plan.redirect_call_with_original("Filter the door object query", kDoorQueryCallRva,
                                                       BytePattern::exact(call), &DoorCorpseFix::query_hook);
}

void DoorCorpseFix::enable_runtime() noexcept {
    active_.publish(*this);
}

void DoorCorpseFix::disable_runtime() noexcept {
    active_.clear(*this);
}

bool DoorCorpseFix::is_dead_soldier(const void* object) const noexcept {
    if (object == nullptr) {
        return false;
    }

    const void* const* vtable{};
    std::memcpy(&vtable, object, sizeof(vtable));
    // Retail GameObject vtable slot 1 identifies the object's derived RTTI type.
    if (vtable == nullptr || vtable[1] != soldier_rtti_getter_) {
        return false;
    }

    std::uint32_t flags{};
    std::memcpy(&flags, static_cast<const std::byte*>(object) + kDamageableFlagsOffset, sizeof(flags));
    // Damageable bit 3 is the authoritative alive state used by the client.
    return (flags & kAliveFlag) == 0;
}

int DoorCorpseFix::query(const float* center, float radius, void** objects, int capacity, void* teams,
                         int affiliation) const noexcept {
    const auto object_query = original_query_.get();
    if (object_query == nullptr) {
        return 0;
    }
    const auto count = object_query(center, radius, objects, capacity, teams, affiliation);
    if (count <= 0 || objects == nullptr || capacity <= 0 || count > capacity) {
        return count;
    }

    // EntityDoor consumes the count, so compact the accepted objects without changing their order.
    auto results = std::span{objects, static_cast<std::size_t>(count)};
    const auto rejected = std::ranges::remove_if(results, [this](const void* object) {
        return is_dead_soldier(object);
    });
    return static_cast<int>(std::distance(results.begin(), rejected.begin()));
}

int __cdecl DoorCorpseFix::query_hook(const float* center, float radius, void** objects, int capacity, void* teams,
                                      int affiliation) noexcept {
    if (const auto* patch = active_.read(); patch != nullptr) {
        return patch->query(center, radius, objects, capacity, teams, affiliation);
    }
    if (const auto original = original_query_.get(); original != nullptr) {
        return original(center, radius, objects, capacity, teams, affiliation);
    }
    return 0;
}

} // namespace fusioncutter::patches::door_corpse_fix
