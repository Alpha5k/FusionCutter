#include "bridge.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

#include "bridge_layout.hpp"

namespace fc::runtime::native {

using namespace bridge_detail;

namespace {

// Describes a prologue stack allocation or saved register for Windows x64 stack unwinding.
struct UnwindOperation {
    // Offset is the completed instruction's prologue offset; operation/information encode the Windows UWOP byte.
    std::uint8_t offset, operation, information;
    // Supported Windows encodings need at most two additional 16-bit allocation or saved-register slots.
    std::array<std::uint16_t, 2> operands{};
    std::uint8_t operand_count{};
};

// Emits the instruction subset needed to marshal native calls and records x64 prologue effects.
class Emitter {
  public:
    // Machine instruction bytes, including a return helper and its address placeholder on x64.
    std::vector<std::uint8_t> code;
    // Prologue operations in emission order; allocation serializes them in reverse order.
    std::vector<UnwindOperation> unwind;
    // Byte length of the prologue reported in UNWIND_INFO.
    std::uint8_t prologue{};
    // Byte offset of the flags-restoring return helper from the generated entry address.
    std::uint32_t return_helper{};
    // Byte offset of the embedded helper address patched after executable allocation.
    std::size_t return_helper_pointer{};

    // Append fixed instruction bytes to the generated code.
    void bytes(std::initializer_list<std::uint8_t> values) {
        code.insert(code.end(), values);
    }

    // Append an immediate or displacement in the executing architecture's little-endian representation.
    template <class T> void value(T input) {
        const auto *begin = reinterpret_cast<const std::uint8_t *>(&input);
        code.insert(code.end(), begin, begin + sizeof(input));
    }

    // Emit the x64 operand-width and extended-register prefix when needed; x86 needs no prefix.
    void rex(bool wide, std::uint8_t reg, std::uint8_t base) {
#if defined(_M_X64)
        // REX.W selects 64-bit operands; REX.R and REX.B extend the ModRM register and base fields.
        const auto prefix = static_cast<std::uint8_t>(0x40 | (wide ? 8 : 0) | (reg >= 8 ? 4 : 0) | (base >= 8 ? 1 : 0));
        if (prefix != 0x40) {
            bytes({prefix});
        }
#else
        static_cast<void>(wide);
        static_cast<void>(reg);
        static_cast<void>(base);
#endif
    }

    // Encode a base-plus-displacement operand; reg also accepts an opcode extension.
    void memory(std::uint8_t reg, std::uint8_t base, std::uint32_t offset) {
        // Use a 32-bit displacement; ESP/RSP bases require an extra SIB byte with no index register.
        bytes({static_cast<std::uint8_t>(0x80 | ((reg & 7) << 3) | (base & 7))});
        if ((base & 7) == 4) {
            bytes({0x24});
        }
        value(offset);
    }

    // Load a one-, two-, four- or eight-byte value from memory into a general register.
    void load(std::uint8_t reg, std::uint8_t base, std::uint32_t offset, std::uint32_t size = kWord) {
        // Zero-extend byte and word values so unused bits of narrow logical inputs stay clear.
        rex(size == 8, reg, base);
        if (size == 1 || size == 2) {
            bytes({0x0f, static_cast<std::uint8_t>(size == 1 ? 0xb6 : 0xb7)});
        } else {
            bytes({0x8b});
        }
        memory(reg, base, offset);
    }

    // Store the selected low bytes of a general register; size must be one, two, four or eight.
    void store(std::uint8_t reg, std::uint8_t base, std::uint32_t offset, std::uint32_t size = kWord) {
        if (size == 2) {
            bytes({0x66});
        }
        rex(size == 8, reg, base);
        bytes({static_cast<std::uint8_t>(size == 1 ? 0x88 : 0x89)});
        memory(reg, base, offset);
    }

    // Compute a base-plus-displacement address without changing processor flags.
    void address(std::uint8_t reg, std::uint8_t base, std::uint32_t offset) {
        rex(kWord == 8, reg, base);
        bytes({0x8d});
        memory(reg, base, offset);
    }

    // Load a pointer-sized constant into a general register.
    void immediate(std::uint8_t reg, std::uintptr_t input) {
        rex(kWord == 8, 0, reg);
        bytes({static_cast<std::uint8_t>(0xb8 + (reg & 7))});
        value(input);
    }

    // Transfer four, eight or sixteen bytes between an XMM register and memory; write selects the destination.
    void simd_memory(std::uint8_t reg, std::uint8_t base, std::uint32_t offset, std::uint32_t size, bool write) {
        // Scalar MOVSS/MOVSD marshal floating values; unaligned MOVDQU saves or restores a full XMM register.
        bytes({static_cast<std::uint8_t>(size == 8 ? 0xf2 : 0xf3)});
        rex(false, reg, base);
        bytes({0x0f, static_cast<std::uint8_t>(size == 16 ? (write ? 0x7f : 0x6f) : (write ? 0x11 : 0x10))});
        memory(reg, base, offset);
    }

    // Reserve or release stack bytes using SUB or ADD, which changes processor flags.
    void stack_adjust(bool subtract, std::uint32_t size) {
        rex(kWord == 8, 0, 4);
        bytes({0x81, static_cast<std::uint8_t>(subtract ? 0xec : 0xc4)});
        value(size);
    }

    // Emit a single-register PUSH using the processor's register number.
    void push(std::uint8_t reg) {
        rex(false, 0, reg);
        bytes({static_cast<std::uint8_t>(0x50 + (reg & 7))});
    }

    // Emit a single-register POP using the processor's register number.
    void pop(std::uint8_t reg) {
        rex(false, 0, reg);
        bytes({static_cast<std::uint8_t>(0x58 + (reg & 7))});
    }

    // Copy size bytes from ESI/RSI to EDI/RDI, advancing both pointers and consuming ECX/RCX.
    void copy(std::uint32_t size) {
        // REP MOVSB uses ECX/RCX as the byte count; CLD selects forward copying.
        bytes({0xb9});
        value(size);
        bytes({0xfc, 0xf3, 0xa4});
    }

    // Address a private buffer with alignment slack; alignment must be a nonzero power of two.
    void aligned_address(std::uint8_t reg, std::uint32_t offset, std::uint32_t alignment) {
        // Round the buffer address upward to its validated power-of-two alignment.
        address(reg, 4, offset + alignment - 1);
        rex(kWord == 8, 0, reg);
        bytes({0x81, static_cast<std::uint8_t>(0xe0 | (reg & 7))});
        value(0U - alignment);
    }

    // Emit a jump and return the operand offset for patch(); condition is a near Jcc opcode or 0xff.
    std::size_t branch(std::uint8_t condition) {
        // Leave a rel32 placeholder until the destination is known; 0xff selects an unconditional jump.
        if (condition == 0xff) {
            bytes({0xe9});
        } else {
            bytes({0x0f, condition});
        }

        const auto position = code.size();
        value(std::int32_t{});
        return position;
    }

    // Resolve a branch() operand to a code offset; throws if the relative displacement does not fit.
    void patch(std::size_t position, std::size_t destination) {
        // Relative branches measure displacement from the end of their four-byte operand.
        const auto distance = static_cast<std::int64_t>(destination) - static_cast<std::int64_t>(position + 4);
        if (distance < INT32_MIN || distance > INT32_MAX) {
            throw std::length_error("Native bridge branch displacement does not fit in a signed 32-bit value");
        }

        const auto encoded = static_cast<std::int32_t>(distance);
        std::memcpy(code.data() + position, &encoded, sizeof(encoded));
    }

    // Record the just-emitted prologue operation; throws if its instruction offset cannot be represented.
    void record(std::uint8_t operation, std::uint8_t information, std::initializer_list<std::uint16_t> operands = {}) {
        // Windows stores the instruction's completed prologue offset in a single byte.
        if (code.size() > 255) {
            throw std::length_error("Native bridge prologue offset exceeds the Windows unwind limit of 255 bytes");
        }
        UnwindOperation description{static_cast<std::uint8_t>(code.size()), operation, information};
        assert(operands.size() <= description.operands.size());
        std::ranges::copy(operands, description.operands.begin());
        description.operand_count = static_cast<std::uint8_t>(operands.size());
        unwind.push_back(description);
    }
};

// Locate a register from kGeneral relative to the saved-stack base; x64 flags occupy the first word.
std::uint32_t saved_offset(std::uint8_t reg) {
    const auto found = std::find(kGeneral.begin(), kGeneral.end(), reg);
    return static_cast<std::uint32_t>(kGeneral.end() - found - (kWord == 8 ? 0 : 1)) * kWord;
}

// Emit an address relative to the saved-register area, independent of private-frame alignment.
void saved_address(Emitter &out, const FrameLayout &frame, std::uint8_t destination, std::uint32_t offset) {
    // x64 has a fixed frame offset; x86 reads the saved pre-alignment ESP anchor.
#if defined(_M_X64)
    out.address(destination, 4, frame.size + offset);
#else
    out.load(destination, 4, frame.anchor);
    out.address(destination, destination, offset);
#endif
}

// Address an incoming stack argument at its normalized byte offset.
void original_stack(Emitter &out, const FrameLayout &frame, std::uint8_t destination, std::uint32_t offset) {
    // Skip saved registers/flags, the return address, and x64 home space to reach physical stack arguments.
    saved_address(out, frame, destination, kSaved + kWord + kHome + offset);
}

// Capture incoming registers and flags, reserve the private frame and record x64 unwind information.
void prologue(Emitter &out, const FrameLayout &frame) {
    // Save physical inputs before using any general register for frame setup or copying.
#if defined(_M_IX86)
    out.bytes({0x9c});
#endif

    for (const auto reg : kGeneral) {
        out.push(reg);
#if defined(_M_X64)
        // Account for each pushed word as an eight-byte allocation; nonvolatile register locations are recorded below.
        out.record(2, 0);
#endif
    }

#if defined(_M_X64)
    out.bytes({0x9c});
    out.record(2, 0);
#endif
#if defined(_M_IX86)
    out.bytes({0x89, 0xe5}); // The original stack anchor survives alignment in a private slot.
#endif

    // Touch each intervening page before reserving a large frame so Windows grows its guarded stack normally.
    if (frame.size >= 4096) {
        out.address(0, 4, 0U - frame.size);
        out.address(1, 4, 0);
        const auto loop = out.code.size();
        out.rex(kWord == 8, 0, 1);
        out.bytes({0x81, 0xe9});
        out.value(std::uint32_t{4096});
        out.rex(kWord == 8, 0, 1);
        out.bytes({0x39, 0xc1});
        const auto done = out.branch(0x86);
        out.bytes({0xf6, 0x01, 0x00});
        const auto again = out.branch(0xff);
        out.patch(again, loop);
        out.patch(done, out.code.size());
        out.bytes({0xf6, 0x00, 0x00});
    }

    // Reserve the private frame and describe the allocation using Windows' scaled or full-size unwind form.
    out.stack_adjust(true, frame.size);
#if defined(_M_X64)
    if (frame.size <= 524280) {
        out.record(1, 0, {static_cast<std::uint16_t>(frame.size / 8)});
    } else {
        out.record(1, 1, {static_cast<std::uint16_t>(frame.size), static_cast<std::uint16_t>(frame.size >> 16)});
    }

    // The pushed nonvolatile values now have fixed offsets; earlier prologue steps have not changed those registers.
    for (const auto reg : kGeneral) {
        if (reg == 3 || reg == 5 || reg == 6 || reg == 7 || reg >= 12) {
            const auto offset = frame.size + saved_offset(reg);
            if (offset <= 524280) {
                out.record(4, reg, {static_cast<std::uint16_t>(offset / 8)});
            } else {
                out.record(5, reg, {static_cast<std::uint16_t>(offset), static_cast<std::uint16_t>(offset >> 16)});
            }
        }
    }
#else
    // Align ESP independently of the caller, retaining the original saved-stack address for restoration.
    out.bytes({0x83, 0xe4, 0xf0});
    out.store(5, 4, frame.anchor);
#endif

    // Capture every SIMD input; x64 unwind records are needed for the nonvolatile XMM6-XMM15 registers.
    for (std::uint8_t reg = 0; reg < kSimdCount; ++reg) {
        out.simd_memory(reg, 4, frame.xmm + reg * 16, 16, true);
#if defined(_M_X64)
        if (reg >= 6) {
            const auto offset = frame.xmm + reg * 16;
            if (offset <= 1048560) {
                out.record(8, reg, {static_cast<std::uint16_t>(offset / 16)});
            } else {
                out.record(9, reg, {static_cast<std::uint16_t>(offset), static_cast<std::uint16_t>(offset >> 16)});
            }
        }
#endif
    }

    out.prologue = static_cast<std::uint8_t>(out.code.size());
    // Copy the original flags from the saved stack into the mutable logical flags slot.
    saved_address(out, frame, 0, kWord == 8 ? 0 : static_cast<std::uint32_t>(kGeneral.size()) * kWord);
    out.load(0, 0, 0);
    out.store(0, 4, frame.flags);
}

// Emit the native return, restoring saved state and applying entry-specific result and cleanup rules.
void restore(Emitter &out, const FrameLayout &frame, const FC_NativeCall &call, bool entry) {
    // Result registers are loaded after restoring nonvolatile SIMD state, so unusual result registers remain visible.
    for (std::uint8_t reg = (kWord == 8 ? 6 : 0); reg < kSimdCount; ++reg) {
        out.simd_memory(reg, 4, frame.xmm + reg * 16, 16, false);
    }

    if (entry && call.result.kind != FC_NATIVE_VOID && !hidden_result(call)) {
        if (const auto reg = simd(call.return_storage.register_id)) {
            out.load(0, 4, frame.result_pointer);
            out.simd_memory(*reg, 0, 0, call.result.size, false);
        } else if (call.return_storage.register_id == FC_REGISTER_ST0) {
            out.load(0, 4, frame.result_pointer);
            out.bytes({static_cast<std::uint8_t>(call.result.size == 8 ? 0xdd : 0xd9), 0x00});
        }
    }

    // Put outgoing flags where POPF can recover them; the x64 helper uses the caller's first home-space word.
    out.load(0, 4, frame.flags);
    saved_address(out, frame, 2, kWord == 8 ? kSaved + kWord : static_cast<std::uint32_t>(kGeneral.size()) * kWord);
    out.store(0, 2, 0);
#if defined(_M_X64)
    out.stack_adjust(false, frame.size + kWord);
#else
    out.load(4, 4, frame.anchor);
#endif

    for (auto index = kGeneral.size(); index != 0; --index) {
        out.pop(kGeneral[index - 1]);
    }

#if defined(_M_X64)
    // A legal unwindable epilogue cannot contain POPFQ. Tail-return through a separately described tiny helper.
    out.bytes({0xff, 0x25, 0x00, 0x00, 0x00, 0x00});
    out.return_helper_pointer = out.code.size();
    out.value(std::uintptr_t{});
    out.return_helper = static_cast<std::uint32_t>(out.code.size());
    out.bytes({0xff, 0x74, 0x24, 0x08, 0x9d, 0xc3});
#else
    out.bytes({0x9d});
    // Entry bridges implement the described x86 return cleanup; invokers return using their own cdecl interface.
    if (entry && call.cleanup == FC_STACK_CLEANUP_CALLEE) {
        out.bytes({0xc2});
        out.value(static_cast<std::uint16_t>(call.stack_size));
    } else {
        out.bytes({0xc3});
    }
#endif
}

// Copy from the current ESI/RSI source into a private-frame stack region; copy() consumes the scratch registers.
void copy_to_stack(Emitter &out, std::uint32_t destination, std::uint32_t size) {
    out.address(7, 4, destination);
    out.copy(size);
}

// Emit a FrameInvoker that marshals logical objects to the validated call layout and copies back its result and flags.
void emit_invoker(Emitter &out, const FC_NativeCall &call, const FrameLayout &frame) {
    prologue(out, frame);

    // Recover the invoker's cdecl target and frame arguments from saved x64 registers or the x86 caller's stack.
#if defined(_M_X64)
    saved_address(out, frame, 0, saved_offset(1));
    out.load(0, 0, 0);
    out.store(0, 4, frame.target);
    saved_address(out, frame, 0, saved_offset(2));
    out.load(0, 0, 0);
#else
    original_stack(out, frame, 0, 0);
    out.load(1, 0, 0);
    out.store(1, 4, frame.target);
    out.load(0, 0, 4);
#endif

    // Preserve borrowed output and flags pointers before scratch registers are reused for marshalling.
    out.store(0, 4, frame.frame);
    out.load(1, 0, offsetof(FC_NativeFrame, result));
    out.store(1, 4, frame.result_pointer);
    out.load(1, 0, offsetof(FC_NativeFrame, flags));
    out.store(1, 4, frame.flag_pointer);
    out.rex(kWord == 8, 1, 1);
    out.bytes({0x85, 0xc9});
    const auto no_flags = out.branch(0x84);
    out.load(1, 1, 0);
    out.store(1, 4, frame.flags);
    out.patch(no_flags, out.code.size());

    // Copy by-value stack inputs before loading registers; REP MOVSB consumes RSI/ESI, RDI/EDI and RCX/ECX.
    for (std::uint32_t index = 0; index < call.argument_count; ++index) {
        const auto &argument = call.arguments[index];
        if (argument.storage.kind != FC_NATIVE_STORAGE_STACK) {
            continue;
        }
        out.load(6, 4, frame.frame);
        out.load(6, 6, offsetof(FC_NativeFrame, arguments));
        out.load(6, 6, index * kWord);
        copy_to_stack(out, kHome + argument.storage.stack_offset, argument.value.size);
    }

    if (hidden_result(call) && call.return_storage.kind == FC_NATIVE_STORAGE_STACK) {
        out.load(0, 4, frame.result_pointer);
        out.store(0, 4, kHome + call.return_storage.stack_offset);
    }

    // Load SIMD inputs before general registers so RAX/EAX remains available for borrowed pointer walks.
    for (std::uint32_t index = 0; index < call.argument_count; ++index) {
        const auto &argument = call.arguments[index];
        if (argument.storage.kind != FC_NATIVE_STORAGE_REGISTER) {
            continue;
        }
        const auto id = argument.storage.register_id;
        if (const auto reg = simd(id)) {
            out.load(0, 4, frame.frame);
            out.load(0, 0, offsetof(FC_NativeFrame, arguments));
            out.load(0, 0, index * kWord);
            out.simd_memory(*reg, 0, 0, argument.value.size, false);
        }
    }

    // Load each general argument through its destination register so later loads preserve earlier argument values.
    for (std::uint32_t index = 0; index < call.argument_count; ++index) {
        const auto &argument = call.arguments[index];
        if (argument.storage.kind != FC_NATIVE_STORAGE_REGISTER) {
            continue;
        }
        if (const auto reg = general(argument.storage.register_id)) {
            out.load(*reg, 4, frame.frame);
            out.load(*reg, *reg, offsetof(FC_NativeFrame, arguments));
            out.load(*reg, *reg, index * kWord);
            out.load(*reg, *reg, 0, argument.value.size);
        }
    }

    // Supply a register-based hidden result pointer after all scratch work; validation keeps its register distinct from inputs.
    if (hidden_result(call) && call.return_storage.kind == FC_NATIVE_STORAGE_REGISTER) {
        out.load(*general(call.return_storage.register_id), 4, frame.result_pointer);
    }

    // Restore incoming flags immediately before the indirect call; memory operands preserve every argument register.
    out.bytes({0xff});
    out.memory(6, 4, frame.flags);
    out.bytes({0x9d});
    out.bytes({0xff});
    out.memory(2, 4, frame.target);
    if (call.cleanup == FC_STACK_CLEANUP_CALLEE) {
        // Undo the callee's stack pop to recover the private frame base without changing its outgoing flags.
        out.address(4, 4, 0U - call.stack_size);
    }

    // Capture callee flags before result copying, stack housekeeping or the dispatcher's observers can alter them.
    out.bytes({0x9c, 0x8f});
    out.memory(0, 4, frame.flags);
    if (call.result.kind != FC_NATIVE_VOID && !hidden_result(call)) {
        if (const auto reg = general(call.return_storage.register_id)) {
            out.store(*reg, 4, frame.result);
            if constexpr (kWord == 4) {
                if (call.result.size == 8) {
                    out.store(2, 4, frame.result + 4);
                }
            }
        } else if (const auto xmm = simd(call.return_storage.register_id)) {
            out.simd_memory(*xmm, 4, frame.result, call.result.size, true);
        } else {
            out.bytes({static_cast<std::uint8_t>(call.result.size == 8 ? 0xdd : 0xd9), 0x9c, 0x24});
            out.value(frame.result);
        }
        out.address(6, 4, frame.result);
        out.load(7, 4, frame.result_pointer);
        out.copy(call.result.size);
    }

    // Publish the callee's flags only when the logical caller supplied an output slot.
    out.load(0, 4, frame.flag_pointer);
    out.rex(kWord == 8, 0, 0);
    out.bytes({0x85, 0xc0});
    const auto skip_flags = out.branch(0x84);
    out.load(1, 4, frame.flags);
    out.store(1, 0, 0);
    out.patch(skip_flags, out.code.size());

    restore(out, frame, call, false);
}

// Emit a native entry that captures the validated call layout and invokes the embedded synchronous callback.
void emit_entry(Emitter &out, const FC_NativeCall &call, const FrameLayout &frame, void *context, FrameCallback callback) {
    prologue(out, frame);

    // Copy each captured physical input into an aligned logical object before calling the dispatcher.
    for (std::uint32_t index = 0; index < call.argument_count; ++index) {
        const auto &argument = call.arguments[index];
        if (argument.storage.kind == FC_NATIVE_STORAGE_STACK) {
            original_stack(out, frame, 6, argument.storage.stack_offset);
        } else if (const auto reg = general(argument.storage.register_id)) {
            saved_address(out, frame, 6, saved_offset(*reg));
        } else if (const auto xmm = simd(argument.storage.register_id)) {
            out.address(6, 4, frame.xmm + *xmm * 16);
        }
        out.aligned_address(7, frame.buffers[index], argument.value.alignment);
        out.store(7, 4, frame.values + index * kWord);
        out.copy(argument.value.size);
    }

    // Hidden results borrow the caller's output object; direct results use aligned private bytes.
    if (hidden_result(call)) {
        if (call.return_storage.kind == FC_NATIVE_STORAGE_STACK) {
            original_stack(out, frame, 0, call.return_storage.stack_offset);
        } else {
            saved_address(out, frame, 0, saved_offset(*general(call.return_storage.register_id)));
        }
        out.load(0, 0, 0);
    } else if (call.result.kind != FC_NATIVE_VOID) {
        out.aligned_address(0, frame.result, call.result.alignment);
    } else {
        out.immediate(0, 0);
    }

    out.store(0, 4, frame.result_pointer);
    // The callback borrows pointers to the captured values, result object, and mutable flags for this invocation.
    out.store(0, 4, frame.frame + offsetof(FC_NativeFrame, result));
    out.address(0, 4, frame.values);
    out.store(0, 4, frame.frame + offsetof(FC_NativeFrame, arguments));
    out.address(0, 4, frame.flags);
    out.store(0, 4, frame.frame + offsetof(FC_NativeFrame, flags));

    // Capture the native return address before the dispatcher calls the target through its own invoker.
#if defined(_M_X64)
    saved_address(out, frame, 8, kSaved);
    out.load(8, 8, 0);
    out.immediate(1, reinterpret_cast<std::uintptr_t>(context));
    out.address(2, 4, frame.frame);
    out.immediate(0, reinterpret_cast<std::uintptr_t>(callback));
    out.bytes({0xff, 0xd0});
#else
    saved_address(out, frame, 0, kSaved);
    out.load(0, 0, 0);
    out.store(0, 4, 8);
    out.address(0, 4, frame.frame);
    out.store(0, 4, 4);
    out.immediate(0, reinterpret_cast<std::uintptr_t>(context));
    out.store(0, 4, 0);
    out.immediate(0, reinterpret_cast<std::uintptr_t>(callback));
    out.bytes({0xff, 0xd0});
#endif

    if (call.result.kind != FC_NATIVE_VOID) {
        // Write general results or the hidden output pointer into saved registers; restore handles SIMD/ST0 results.
        if (hidden_result(call)) {
            out.load(0, 4, frame.result_pointer);
            saved_address(out, frame, 2, saved_offset(0));
            out.store(0, 2, 0);
        } else if (const auto reg = general(call.return_storage.register_id)) {
            out.load(6, 4, frame.result_pointer);
            saved_address(out, frame, 7, saved_offset(*reg));
            out.copy(std::min(call.result.size, kWord));
            if constexpr (kWord == 4) {
                if (call.result.size == 8) {
                    out.load(6, 4, frame.result_pointer);
                    out.address(6, 6, 4);
                    saved_address(out, frame, 7, saved_offset(2));
                    out.copy(4);
                }
            }
        }
    }

    restore(out, frame, call, true);
}

// Publish emitted instructions as executable memory and return its x64 unwind registration through unwind_table.
// Failures release the allocation and return a diagnostic; success transfers ownership to the caller.
std::expected<void *, std::string> allocate(Emitter &out, void *&unwind_table) {
    if (out.code.empty() || out.code.size() > UINT32_MAX) {
        return std::unexpected("Native bridge instruction bytes are empty or exceed the 32-bit size limit");
    }

    auto bytes = out.code;
#if defined(_M_X64)
    // UNWIND_INFO follows the instructions at a four-byte-aligned offset within the same allocation.
    while (bytes.size() % 4 != 0) {
        bytes.push_back(0);
    }

    const auto unwind_offset = static_cast<DWORD>(bytes.size());
    std::size_t count{};
    for (const auto &operation : out.unwind) {
        count += 1 + operation.operand_count;
    }
    if (count > 255) {
        return std::unexpected("Native bridge unwind metadata requires more than 255 code slots");
    }

    bytes.insert(bytes.end(), {1, out.prologue, static_cast<std::uint8_t>(count), 0});

    // Windows reads unwind operations in descending prologue order; operand words count as additional slots.
    for (auto index = out.unwind.size(); index != 0; --index) {
        const auto &operation = out.unwind[index - 1];
        bytes.push_back(operation.offset);
        bytes.push_back(static_cast<std::uint8_t>(operation.operation | (operation.information << 4)));
        for (const auto operand : std::span{operation.operands}.first(operation.operand_count)) {
            bytes.push_back(static_cast<std::uint8_t>(operand));
            bytes.push_back(static_cast<std::uint8_t>(operand >> 8));
        }
    }

    while (bytes.size() % alignof(RUNTIME_FUNCTION) != 0) {
        bytes.push_back(0);
    }

    // Pad the main description before placing the helper's description and the address-sorted function table.
    const auto helper_unwind = static_cast<DWORD>(bytes.size());
    bytes.insert(bytes.end(), {1, 4, 1, 0, 4, 2, 0, 0});
    const auto table_offset = bytes.size();
    const std::array functions{RUNTIME_FUNCTION{0, out.return_helper, unwind_offset},
                               RUNTIME_FUNCTION{out.return_helper, static_cast<DWORD>(out.code.size()), helper_unwind}};
    const auto *data = reinterpret_cast<const std::uint8_t *>(functions.data());
    bytes.insert(bytes.end(), data, data + sizeof(functions));
#endif

    if (bytes.size() > UINT32_MAX) {
        return std::unexpected("Native bridge allocation exceeds the 32-bit limit for relative addresses");
    }

    // Fill writable memory first, then make the completed instructions executable and flush the instruction cache.
    void *allocation = VirtualAlloc(nullptr, bytes.size(), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!allocation) {
        return std::unexpected("Could not allocate memory for the native bridge (Windows error " + std::to_string(GetLastError()) + ")");
    }
    std::memcpy(allocation, bytes.data(), bytes.size());
#if defined(_M_X64)
    // The indirect tail jump needs the helper's absolute address, which is known only after allocation.
    const auto helper = reinterpret_cast<std::uintptr_t>(allocation) + out.return_helper;
    std::memcpy(static_cast<std::uint8_t *>(allocation) + out.return_helper_pointer, &helper, sizeof(helper));
#endif

    DWORD previous{};
    if (!VirtualProtect(allocation, bytes.size(), PAGE_EXECUTE_READ, &previous) ||
        !FlushInstructionCache(GetCurrentProcess(), allocation, out.code.size())) {
        const auto error = GetLastError();
        VirtualFree(allocation, 0, MEM_RELEASE);
        return std::unexpected("Could not make native bridge memory executable or flush its instruction cache (Windows error " +
                               std::to_string(error) + ")");
    }

#if defined(_M_X64)
    auto *table = reinterpret_cast<PRUNTIME_FUNCTION>(static_cast<std::uint8_t *>(allocation) + table_offset);
    // Register the nonleaf body and flags helper together; their owner unregisters both before freeing code.
    if (!RtlAddFunctionTable(table, 2, reinterpret_cast<DWORD64>(allocation))) {
        VirtualFree(allocation, 0, MEM_RELEASE);
        return std::unexpected("Could not register native bridge unwind metadata");
    }
    unwind_table = table;
#else
    static_cast<void>(unwind_table);
#endif

    return allocation;
}

// Free a bridge and its optional unwind registration after every invocation has finished.
void release(void *allocation, void *unwind_table) noexcept {
#if defined(_M_X64)
    // Remove Windows' references to unwind metadata before freeing the memory that contains it.
    if (unwind_table) {
        RtlDeleteFunctionTable(static_cast<PRUNTIME_FUNCTION>(unwind_table));
    }
#else
    static_cast<void>(unwind_table);
#endif

    if (allocation) {
        VirtualFree(allocation, 0, MEM_RELEASE);
    }
}

} // namespace

ExecutableBridge::ExecutableBridge() noexcept = default;

ExecutableBridge::ExecutableBridge(ExecutableBridge &&other) noexcept
    : allocation_(std::exchange(other.allocation_, nullptr)), size_(std::exchange(other.size_, 0)),
      unwind_table_(std::exchange(other.unwind_table_, nullptr)) {}

ExecutableBridge &ExecutableBridge::operator=(ExecutableBridge &&other) noexcept {
    if (this != &other) {
        // Release the current code and transfer both the executable allocation and its unwind registration.
        release(allocation_, unwind_table_);
        allocation_ = std::exchange(other.allocation_, nullptr);
        size_ = std::exchange(other.size_, 0);
        unwind_table_ = std::exchange(other.unwind_table_, nullptr);
    }
    return *this;
}

ExecutableBridge::~ExecutableBridge() {
    release(allocation_, unwind_table_);
}

std::uintptr_t ExecutableBridge::address() const noexcept {
    return reinterpret_cast<std::uintptr_t>(allocation_);
}

std::size_t ExecutableBridge::size() const noexcept {
    return size_;
}

std::expected<ExecutableBridge, std::string> ExecutableBridge::invoker(const FC_NativeCall &call) {
    try {
        // Emit against a checked private layout, then publish code with its matching unwind metadata.
        Emitter out;
        emit_invoker(out, call, layout(call, false));

        ExecutableBridge result;
        auto allocation = allocate(out, result.unwind_table_);
        if (!allocation) {
            return std::unexpected(allocation.error());
        }

        result.allocation_ = *allocation;
        result.size_ = out.code.size();
        return result;
    } catch (const std::exception &error) {
        return std::unexpected(error.what());
    }
}

std::expected<ExecutableBridge, std::string> ExecutableBridge::entry(const FC_NativeCall &call, void *context, FrameCallback callback) {
    if (!callback) {
        return std::unexpected("Native bridge entry requires a dispatcher callback");
    }

    try {
        // Bind context and callback addresses into the emitted entry before publishing its executable owner.
        Emitter out;
        emit_entry(out, call, layout(call, true), context, callback);

        ExecutableBridge result;
        auto allocation = allocate(out, result.unwind_table_);
        if (!allocation) {
            return std::unexpected(allocation.error());
        }

        result.allocation_ = *allocation;
        result.size_ = out.code.size();
        return result;
    } catch (const std::exception &error) {
        return std::unexpected(error.what());
    }
}

} // namespace fc::runtime::native
