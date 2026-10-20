#include "hook_site.hpp"
#include "../fatal_boundary.hpp"
#include "patches/patch_api.hpp"
#include "native_address.hpp"

#include <Winsock2.h>
#include <Windows.h>
#include <intrin.h>
#include <safetyhook/inline_hook.hpp>
#include <safetyhook/mid_hook.hpp>
#include <safetyhook/os.hpp>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstring>
#include <new>
#include <utility>

namespace fc::runtime::hook_detail {

namespace {

thread_local std::uintptr_t gReturnAddress{};

// Restores the enclosing invocation's address after a nested hook returns on this thread.
class ReturnAddressScope {
  public:
    // Publishes this invocation's caller until the scope ends, preserving any enclosing call.
    explicit ReturnAddressScope(std::uintptr_t address) noexcept : previous_(std::exchange(gReturnAddress, address)) {}

    ~ReturnAddressScope() {
        gReturnAddress = previous_;
    }

  private:
    std::uintptr_t previous_;
};

// Applies patch ownership and the callback exception boundary before invoking an author's thunk.
void invoke_author(const Participant &participant, FC_NativeThunk callback, const FC_NativeFrame &frame, void *state) {
    PatchCallbackScope scope{participant.plan->owner()};
    invoke_patch_callback([&] {
        callback(participant.operation->value.context, &frame, state);
    });
}

// Saves floating-point state and thread error values around author callbacks.
struct Ambient {
    // FXSAVE requires sixteen-byte alignment and a 512-byte destination.
    alignas(16) unsigned char floating[512]{};
    DWORD error;
    int socket;

    // Captures this thread's error values and x87/SSE state for restoration between callbacks.
    Ambient() noexcept : error(GetLastError()), socket(WSAGetLastError()) {
        _fxsave(floating);
    }

    // Restores the captured state; Windows' last error is set after the Winsock error setter.
    void restore() const noexcept {
        _fxrstor(const_cast<unsigned char *>(floating));
        WSASetLastError(socket);
        SetLastError(error);
    }
};

// One aligned block belongs to one invocation; recursive and concurrent entries allocate independently.
struct InvocationState {
    void *storage{};
    const Participants *participants{};

    // Allocates and initializes state for one captured list, which must outlive this invocation.
    explicit InvocationState(const Participants &list) : participants(&list) {
        if (!list.state_size) {
            return;
        }

        // Every state-bearing declaration supplies an initializer that begins its state lifetime.
        storage = ::operator new(list.state_size, std::align_val_t{list.state_alignment});
        for (const auto &item : list.values) {
            if (item.operation->value.state_size) {
                PatchCallbackScope scope{item.plan->owner()};
                invoke_patch_callback([&] {
                    item.operation->value.initialize(value(item));
                });
            }
        }
    }

    // Returns this participant's aligned slice, or null when it declared no state.
    void *value(const Participant &item) const {
        return item.operation->value.state_size ? static_cast<std::byte *>(storage) + item.state_offset : nullptr;
    }

    // Frees the shared block once; callbacks must have finished using their state slices.
    void release() noexcept {
        if (storage) {
            ::operator delete(storage, std::align_val_t{participants->state_alignment});
            storage = nullptr;
        }
    }

    ~InvocationState() {
        release();
    }
};

// Invokes a present callback with the supplied ambient snapshot and this participant's invocation state.
void invoke_observer(const Participant &participant, FC_NativeThunk callback, const FC_NativeFrame &frame, void *state,
                     const Ambient &ambient) {
    if (callback) {
        // Every observer starts with the same saved state, regardless of changes made by earlier callbacks.
        ambient.restore();
        invoke_author(participant, callback, frame, state);
    }
}

} // namespace

std::uintptr_t FC_CDECL current_return_address() noexcept {
    return gReturnAddress;
}

void FC_CDECL typed_dispatch(void *context, const FC_NativeFrame *frame, std::uintptr_t return_address) noexcept {
    try {
        auto &site = *static_cast<HookSite *>(context);
        // Preparation publishes a retained list before the physical entry can execute.
        const auto &list = *site.current.load(std::memory_order_acquire);

        // This invocation uses one retained list across before callbacks, the original/modifier and after callbacks.
        Ambient incoming;
        ReturnAddressScope address{return_address};
        InvocationState states{list};
        for (std::size_t i = 0; i < list.values.size(); ++i) {
            invoke_observer(list.values[i], list.values[i].operation->value.before, *frame, states.value(list.values[i]), incoming);
        }

        // The modifier replaces the original call; observers alone leave it callable through the invoker.
        incoming.restore();
        if (list.modifier) {
            invoke_author(*list.modifier, list.modifier->operation->value.modifier, *frame, nullptr);
        } else {
            reinterpret_cast<native::FrameInvoker>(site.invoker.address())(site.original_target, frame);
        }

        // After observers see the completed call and unwind the before order.
        Ambient outgoing;
        for (std::size_t i = list.values.size(); i; --i) {
            invoke_observer(list.values[i - 1], list.values[i - 1].operation->value.after, *frame, states.value(list.values[i - 1]),
                            outgoing);
        }

        // Free invocation storage before restoring the completed call's state for the native return.
        states.release();
        outgoing.restore();
    } catch (...) {
        fatal_invariant("Installed native hook dispatch failed");
    }
}

void FC_CDECL instruction_dispatch(void *context, const FC_NativeFrame *frame, std::uintptr_t) noexcept {
    try {
        // An instruction snapshot cannot establish the enclosing native function's return address.
        ReturnAddressScope address{0};
        auto &site = *static_cast<HookSite *>(context);

        // Copy backend registers into the ABI context that author callbacks inspect and modify.
        auto &backend = **static_cast<safetyhook::Context *const *>(frame->arguments[0]);
        FC_CpuContext cpu{};
#if defined(_WIN64)
        std::memcpy(cpu.xmm, &backend.xmm0, sizeof(cpu.xmm));
        cpu.rflags = backend.rflags;
        cpu.r15 = backend.r15;
        cpu.r14 = backend.r14;
        cpu.r13 = backend.r13;
        cpu.r12 = backend.r12;
        cpu.r11 = backend.r11;
        cpu.r10 = backend.r10;
        cpu.r9 = backend.r9;
        cpu.r8 = backend.r8;
        cpu.rdi = backend.rdi;
        cpu.rsi = backend.rsi;
        cpu.rdx = backend.rdx;
        cpu.rcx = backend.rcx;
        cpu.rbx = backend.rbx;
        cpu.rax = backend.rax;
        cpu.rbp = backend.rbp;
        cpu.rsp = backend.rsp;
        cpu.rip = backend.rip;
        const auto original_stack = cpu.rsp;
#else
        std::memcpy(cpu.xmm, &backend.xmm0, sizeof(cpu.xmm));
        cpu.eflags = backend.eflags;
        cpu.edi = backend.edi;
        cpu.esi = backend.esi;
        cpu.edx = backend.edx;
        cpu.ecx = backend.ecx;
        cpu.ebx = backend.ebx;
        cpu.eax = backend.eax;
        cpu.ebp = backend.ebp;
        cpu.esp = backend.esp;
        cpu.eip = backend.eip;
        const auto original_stack = cpu.esp;
#endif

        // Instruction callbacks receive one logical argument: a pointer to the mutable CPU context.
        const auto *cpu_pointer = &cpu;
        const void *arguments[]{&cpu_pointer};
        FC_NativeFrame logical{arguments, nullptr, nullptr};

        Ambient incoming;
        // Preparation publishes a retained list before the physical entry can execute.
        const auto &list = *site.current.load(std::memory_order_acquire);
        InvocationState states{list};
        for (const auto &item : list.values) {
            invoke_observer(item, item.operation->value.before, logical, states.value(item), incoming);
        }

        incoming.restore();
        // The modifier edits the context after all before observers have examined it.
        if (list.modifier) {
            invoke_author(*list.modifier, list.modifier->operation->value.modifier, logical, states.value(*list.modifier));
        }

        // After observers inspect the edited context in reverse participant order.
        Ambient outgoing;
        for (auto item = list.values.rbegin(); item != list.values.rend(); ++item) {
            invoke_observer(*item, item->operation->value.after, logical, states.value(*item), outgoing);
        }

        states.release();
        outgoing.restore();

        // Apply register edits after callbacks finish; a changed stack needs a new return slot.
#if defined(_WIN64)
        std::memcpy(&backend.xmm0, cpu.xmm, sizeof(cpu.xmm));
        backend.rflags = cpu.rflags;
        backend.r15 = cpu.r15;
        backend.r14 = cpu.r14;
        backend.r13 = cpu.r13;
        backend.r12 = cpu.r12;
        backend.r11 = cpu.r11;
        backend.r10 = cpu.r10;
        backend.r9 = cpu.r9;
        backend.r8 = cpu.r8;
        backend.rdi = cpu.rdi;
        backend.rsi = cpu.rsi;
        backend.rdx = cpu.rdx;
        backend.rcx = cpu.rcx;
        backend.rbx = cpu.rbx;
        backend.rax = cpu.rax;
        backend.rbp = cpu.rbp;
        backend.rip = cpu.rip;

        if (cpu.rsp != original_stack) {
            // The backend resumes by returning through the slot immediately below the requested stack pointer.
            if (!patching::check_native_access(cpu.rsp - sizeof(void *), sizeof(void *), true)) {
                fatal_invariant("Could not verify write access to the return slot below the instruction hook's new stack pointer");
            }
            backend.trampoline_rsp = cpu.rsp - sizeof(void *);
            std::memcpy(reinterpret_cast<void *>(backend.trampoline_rsp), &cpu.rip, sizeof(cpu.rip));
        }
#else
        std::memcpy(&backend.xmm0, cpu.xmm, sizeof(cpu.xmm));
        backend.eflags = cpu.eflags;
        backend.edi = cpu.edi;
        backend.esi = cpu.esi;
        backend.edx = cpu.edx;
        backend.ecx = cpu.ecx;
        backend.ebx = cpu.ebx;
        backend.eax = cpu.eax;
        backend.ebp = cpu.ebp;
        backend.eip = cpu.eip;

        if (cpu.esp != original_stack) {
            // The backend resumes by returning through the slot immediately below the requested stack pointer.
            if (!patching::check_native_access(cpu.esp - sizeof(void *), sizeof(void *), true)) {
                fatal_invariant("Could not verify write access to the return slot below the instruction hook's new stack pointer");
            }
            backend.trampoline_esp = cpu.esp - sizeof(void *);
            std::memcpy(reinterpret_cast<void *>(backend.trampoline_esp), &cpu.eip, sizeof(cpu.eip));
        }
#endif
    } catch (...) {
        fatal_invariant("Installed instruction hook dispatch failed");
    }
}

} // namespace fc::runtime::hook_detail
