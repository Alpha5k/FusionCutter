#pragma once

#include <cstddef>
#include <span>

namespace fusioncutter::patches::update_recovery {

// Stable-order a receive batch by host turn without allocating in the game callback.
template <typename Update, std::size_t Extent> void order_updates_by_turn(std::span<Update, Extent> updates) noexcept {
    for (std::size_t index = 1; index < updates.size(); ++index) {
        auto update = updates[index];
        auto insertion = index;
        while (insertion > 0 && updates[insertion - 1].turn > update.turn) {
            updates[insertion] = updates[insertion - 1];
            --insertion;
        }
        updates[insertion] = update;
    }
}

} // namespace fusioncutter::patches::update_recovery
