#include "transport.hpp"

#include "layout.hpp"

#include <array>
#include <bit>
#include <cstdio>
#include <utility>

namespace fusioncutter::patches::direct_transport::server {

void ServerTransport::build_plan(PatchPlan& plan) const {
    plan.require_bytes("Validate player state query", layout::kIsPlayingRva,
                       BytePattern::exact(layout::kIsPlayingPreimage));
    plan.require_bytes("Validate player removal update", layout::kUpdateBootPlayerRva,
                       BytePattern::exact(layout::kUpdateBootPlayerPreimage));
    plan.require_bytes("Validate disconnected player update", layout::kSetNotPlayingRva,
                       BytePattern::exact(layout::kSetNotPlayingPreimage));
}

void ServerTransport::service_removals() noexcept {
    if (!has_pending_removals_) {
        return;
    }
    using IsPlaying = bool(__cdecl*)(int, bool);
    using UpdateBootPlayer = void(__cdecl*)(int*);
    const auto is_playing = image_.function_at_rva<IsPlaying>(layout::kIsPlayingRva);
    const auto update_boot_player = image_.function_at_rva<UpdateBootPlayer>(layout::kUpdateBootPlayerRva);
    bool remaining{};
    for (auto& player : pending_removals_) {
        if (player < 0) {
            continue;
        }
        if (!is_playing(player, false)) {
            player = -1;
            continue;
        }
        update_boot_player(&player);
        remaining |= player >= 0;
    }
    has_pending_removals_ = remaining;
}

void ServerTransport::remove_peer(std::uint8_t physical_primary, RemovalReason reason) noexcept {
    constexpr std::array<const char*, std::to_underlying(RemovalReason::Count)> kRemovalReasons{
        "control message limit exceeded",
        "unsupported Direct Transport protocol",
        "Direct Transport is required",
        "direct negotiation failed",
    };
    if (physical_primary >= associations_.size() || !associations_[physical_primary].live) {
        return;
    }
    const auto reason_index = std::to_underlying(reason);
    const auto occurrence = ++removal_counts_[reason_index];
    if (std::has_single_bit(occurrence)) {
        char message[144]{};
        std::snprintf(message, sizeof(message), "Player slot %u removed: %s (occurrence %llu)", physical_primary,
                      kRemovalReasons[reason_index], static_cast<unsigned long long>(occurrence));
        logging::warning("DirectTransport", message, "Enforce server transport policy");
    }
    associations_[physical_primary].diagnostics.mark_policy_action();
    rearm_blocked_[physical_primary] = true;
    invalidate_association(physical_primary, reason == RemovalReason::ControlLimit
                                                 ? AssociationEndReason::ControlLimit
                                                 : AssociationEndReason::PolicyRemoval);
    using IsPlaying = bool(__cdecl*)(int, bool);
    using SetNotPlaying = void(__cdecl*)(int);
    const auto is_playing = image_.function_at_rva<IsPlaying>(layout::kIsPlayingRva);
    if (is_playing(physical_primary, false)) {
        pending_removals_[physical_primary] = physical_primary;
        has_pending_removals_ = true;
    } else {
        image_.function_at_rva<SetNotPlaying>(layout::kSetNotPlayingRva)(physical_primary);
    }
}

} // namespace fusioncutter::patches::direct_transport::server
