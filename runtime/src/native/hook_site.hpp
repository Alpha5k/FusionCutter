#pragma once

#include "hook_registry.hpp"

#include <safetyhook/inline_hook.hpp>
#include <safetyhook/mid_hook.hpp>

#include <atomic>
#include <span>

namespace fc::runtime {

// Connects one retained plan declaration to its state within a shared hook invocation.
struct Participant {
    // Installed plans remain alive while their callbacks or original-call handles can be used.
    SubmittedPlan *plan{};
    const HookParticipant *operation{};
    // Byte offset into the invocation's aligned state block.
    std::size_t state_offset{};
};

// Describes an immutable callback order and the storage required for one invocation.
struct Participants {
    std::vector<Participant> values;
    // Points to this list's modifier in finalized values, or is null when none was declared.
    const Participant *modifier{};
    // Total state bytes and block alignment, at least alignof(std::max_align_t).
    std::size_t state_size{}, state_alignment = alignof(std::max_align_t);
};

// Owns one physical detour and the bridges and participant lists used by calls through it.
struct HookSite {
    // Absolute detour address and callable original target, which may be a function trampoline.
    std::uintptr_t address{}, original_target{};
    std::uint32_t scope{};
    // Borrows the first participant's typed call description; instruction hooks leave it null.
    const CallDescription *call{};
    // Borrows immutable original bytes from the retained first declaration.
    std::span<const std::byte> original;
    // Owns the live detour bytes checked before reconstructing an original view.
    std::vector<std::byte> installed;
    // Entry captures a native frame; invoker calls the original using its declared storage layout.
    native::ExecutableBridge entry, invoker;
    // Keeps a distant callsite entry reachable through a nearby indirect jump.
    patching::NativeAllocation relay;
    // Dispatch captures one list; older lists stay alive for invocations already using them.
    std::atomic<const Participants *> current{};
    std::vector<std::unique_ptr<Participants>> lists;
    // Backend detours detach before the participant lists and executable bridges are destroyed.
    safetyhook::InlineHook function;
    safetyhook::MidHook instruction;
};

namespace hook_detail {

// Returns the native return address for this thread's active typed invocation, or zero when unavailable.
std::uintptr_t FC_CDECL current_return_address() noexcept;

// Runs before observers, the modifier or original call, then after observers in reverse order.
// The entry bridge supplies a live HookSite and a borrowed frame valid for this synchronous invocation.
void FC_CDECL typed_dispatch(void *context, const FC_NativeFrame *frame, std::uintptr_t return_address) noexcept;
// Exposes the backend register snapshot to callbacks and copies their edits into the continuation.
// The entry bridge supplies a live HookSite and a frame whose first argument holds the backend context pointer.
void FC_CDECL instruction_dispatch(void *context, const FC_NativeFrame *frame, std::uintptr_t return_address) noexcept;

} // namespace hook_detail

} // namespace fc::runtime
