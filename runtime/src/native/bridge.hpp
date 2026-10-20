#pragma once

#include <fc/abi.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>

namespace fc::runtime::native {

// The callback borrows the frame and its argument/result objects for one synchronous invocation.
// The final argument is the original caller's return address, captured before invoking any bridge callback.
using FrameCallback = void(FC_CDECL *)(void *, const FC_NativeFrame *, std::uintptr_t return_address) noexcept;

// Invoke a matching native target with a nonnull frame and valid argument/result objects for the described sizes.
// Objects are borrowed for the call; optional frame.flags supplies incoming flags and receives callee flags.
using FrameInvoker = void(FC_CDECL *)(std::uintptr_t target, const FC_NativeFrame *) noexcept;

// Owns executable instructions that convert between logical frames and described physical native calls.
class ExecutableBridge {
  public:
    ExecutableBridge() noexcept;
    ExecutableBridge(const ExecutableBridge &) = delete;
    ExecutableBridge &operator=(const ExecutableBridge &) = delete;
    // Transfer executable storage and its unwind registration, leaving the source empty.
    ExecutableBridge(ExecutableBridge &&) noexcept;
    // Release current storage and transfer ownership; current invocations must have finished first.
    ExecutableBridge &operator=(ExecutableBridge &&) noexcept;
    // Release executable storage and unwind registration after the address is unreachable and invocations have finished.
    ~ExecutableBridge();

    // Generate a FrameInvoker from a description valid for validate_call(call, true); failures return a diagnostic.
    static std::expected<ExecutableBridge, std::string> invoker(const FC_NativeCall &);

    // Generate an entry from a description valid for validate_call(call, true); failures return a diagnostic.
    // Callers use its physical convention; context/callback remain valid during every synchronous callback invocation.
    static std::expected<ExecutableBridge, std::string> entry(const FC_NativeCall &, void *context, FrameCallback);

    // Return the executable entry address, or zero when empty; cast invokers to FrameInvoker.
    // Retain this owner until the address is unreachable and all invocations have returned.
    std::uintptr_t address() const noexcept;
    // Returns the instruction byte count, excluding appended unwind metadata.
    std::size_t size() const noexcept;

  private:
    // VirtualAlloc storage contains instructions and, on x64, their unwind records and function table.
    void *allocation_{};
    // Instruction bytes published by size(); zero for an empty or moved-from owner.
    std::size_t size_{};
    // Registered x64 function table inside allocation_; removed before releasing that allocation.
    void *unwind_table_{};
};

// Check physical storage and cleanup support; call must already have valid logical values and a readable argument view.
// Returns a diagnostic for unsupported layouts.
std::expected<void, std::string> validate_bridge_layout(const FC_NativeCall &);

} // namespace fc::runtime::native
