#pragma once

#include <cstdint>

namespace fusioncutter::patches::network_diagnostics {

// Adds a recovered client-update batch to the active network trace.
void observe_update_recovery(std::uint32_t updates, std::uint32_t oldest_turn, std::uint32_t newest_turn) noexcept;

// Folds server scheduling decisions into the current send transaction.
void observe_ack_slot(std::int32_t slot, bool accepted) noexcept;
void observe_create_fence(std::uint32_t destination, std::int32_t turn) noexcept;
void observe_destination_gate(std::uint32_t destination, bool blocked, bool timed_out) noexcept;
void observe_object_budget(std::uint32_t destination, std::uint32_t pending_events, std::int32_t object_scale) noexcept;

} // namespace fusioncutter::patches::network_diagnostics
