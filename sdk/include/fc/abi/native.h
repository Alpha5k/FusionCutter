#pragma once

#include <fc/abi/common.h>

#ifdef __cplusplus
extern "C" {
#endif

// Locations use selected profile IDs; names only describe the operation in diagnostics.
typedef struct FC_Location {
    // Selected profile ID, compared ASCII case-insensitively.
    FC_Text image;
    // Relative virtual address (RVA): byte offset from the loaded image base.
    uint32_t rva;
    FC_Text name;
    // Nonzero records a failed derivation and causes submission to reject this location.
    uint32_t invalid;
} FC_Location;

// Selects the preimage comparison required when the native request is planned and revalidated.
enum FC_EvidenceKind {
    FC_EVIDENCE_NONE,
    FC_EVIDENCE_EXACT_BYTES,
    FC_EVIDENCE_MASKED_BYTES,
    FC_EVIDENCE_POINTS_TO,
    FC_EVIDENCE_DIRECT_CALL_TO,
    FC_EVIDENCE_DIRECT_JUMP_TO
};

// Borrows comparison bytes or an expected target; submission copies retained views.
typedef struct FC_Evidence {
    uint32_t kind;
    // Masked evidence selects bits with equal-length nonempty bytes/mask arrays; exact evidence uses all bits.
    FC_Bytes bytes, mask;
    // Expected destination for pointer-sized fields or five-byte direct relative CALL/JMP instructions.
    FC_Location target;
} FC_Evidence;

// Selects an image location, a replacement-data allocation or executable code in the submitting plugin.
enum FC_AddressKind {
    FC_ADDRESS_IMAGE,
    FC_ADDRESS_ALLOCATION,
    FC_ADDRESS_FUNCTION
};

// Referenced image targets retain their own evidence, independently of the destination's evidence.
typedef struct FC_Address {
    uint32_t kind;
    FC_Location location;
    FC_Evidence evidence;
    // Opaque allocation from this plan; offset is measured in bytes and may designate its end.
    void *allocation;
    uint64_t offset;
    // Absolute process address used by FC_ADDRESS_FUNCTION.
    uintptr_t function;
    uint32_t invalid;
} FC_Address;

// Identifies the machine ABI described by a native call.
enum FC_Architecture {
    FC_ARCH_X86 = 1,
    FC_ARCH_X64 = 2
};

// Classifies a logical argument or result independently of its physical register/stack storage.
enum FC_NativeKind {
    FC_NATIVE_VOID,
    FC_NATIVE_INTEGER,
    FC_NATIVE_POINTER,
    FC_NATIVE_FLOAT_32,
    FC_NATIVE_FLOAT_64,
    FC_NATIVE_RECORD
};

// Selects physical storage; NONE is used for void results and unassigned locations.
enum FC_NativeStorageKind {
    FC_NATIVE_STORAGE_NONE,
    FC_NATIVE_STORAGE_REGISTER,
    FC_NATIVE_STORAGE_STACK
};

// Names physical registers accepted by explicit call layouts for the selected architecture.
enum FC_NativeRegister {
    FC_REGISTER_NONE,
    FC_REGISTER_EAX,
    FC_REGISTER_EBX,
    FC_REGISTER_ECX,
    FC_REGISTER_EDX,
    FC_REGISTER_ESI,
    FC_REGISTER_EDI,
    FC_REGISTER_EBP,
    FC_REGISTER_ST0,
    FC_REGISTER_RAX = 16,
    FC_REGISTER_RBX,
    FC_REGISTER_RCX,
    FC_REGISTER_RDX,
    FC_REGISTER_RSI,
    FC_REGISTER_RDI,
    FC_REGISTER_RBP,
    FC_REGISTER_R8,
    FC_REGISTER_R9,
    FC_REGISTER_R10,
    FC_REGISTER_R11,
    FC_REGISTER_R12,
    FC_REGISTER_R13,
    FC_REGISTER_R14,
    FC_REGISTER_R15,
    FC_REGISTER_XMM0 = 32,
    FC_REGISTER_XMM1,
    FC_REGISTER_XMM2,
    FC_REGISTER_XMM3,
    FC_REGISTER_XMM4,
    FC_REGISTER_XMM5,
    FC_REGISTER_XMM6,
    FC_REGISTER_XMM7,
    FC_REGISTER_XMM8,
    FC_REGISTER_XMM9,
    FC_REGISTER_XMM10,
    FC_REGISTER_XMM11,
    FC_REGISTER_XMM12,
    FC_REGISTER_XMM13,
    FC_REGISTER_XMM14,
    FC_REGISTER_XMM15
};

// Specifies who removes x86 stack argument bytes; x64 descriptions use NONE.
enum FC_StackCleanup {
    FC_STACK_CLEANUP_NONE,
    FC_STACK_CLEANUP_CALLER,
    FC_STACK_CLEANUP_CALLEE
};

// Selects compiler calling conventions, variadic direct calls and hidden result-pointer passing.
enum FC_CallFlags {
    // Compiler describes argument passing; hidden result representation is selected independently.
    FC_CALL_COMPILER = 1,
    FC_CALL_VARIADIC = 2,
    FC_CALL_HIDDEN_RESULT = 4
};

// Describes one logical value; size and alignment are in bytes, and void uses zero for both.
typedef struct FC_NativeValue {
    uint32_t kind, size, alignment;
} FC_NativeValue;

// Describes one physical location; stack_offset is in bytes after the return address and x64 home area.
typedef struct FC_NativeStorage {
    uint32_t kind, register_id, stack_offset;
} FC_NativeStorage;

// Pairs a logical value description with the physical location used by the native function.
typedef struct FC_NativeArgument {
    FC_NativeValue value;
    FC_NativeStorage storage;
} FC_NativeArgument;

// Describes logical values and their physical locations for a retained call or typed hook.
typedef struct FC_NativeCall {
    uint32_t architecture, flags;
    FC_NativeValue result;
    // A hidden result location carries a pointer to logical result storage; other results occupy this location directly.
    FC_NativeStorage return_storage;
    // Logical argument order; the array is borrowed until the runtime copies the description.
    const FC_NativeArgument *arguments;
    // stack_size is the normalized argument area's byte extent, excluding return address and x64 home space.
    uint32_t argument_count, cleanup, stack_size;
} FC_NativeCall;

// Thunks borrow synchronous logical argument/result storage supplied by the runtime or SDK caller.
// Argument pointers follow logical order; result is null for void and optional processor flags storage is input/output.
typedef struct FC_NativeFrame {
    const void *const *arguments;
    void *result;
    uintptr_t *flags;
} FC_NativeFrame;

// Receives the retained callback context, synchronous logical frame and optional per-invocation state; must not throw.
typedef void(FC_CDECL *FC_NativeThunk)(void *, const FC_NativeFrame *, void *state);
// Begins state lifetime in aligned runtime storage for one hook invocation; must not throw.
typedef void(FC_CDECL *FC_StateInitialize)(void *);

// Holds the low and high 64-bit halves of one saved 128-bit XMM register.
typedef struct FC_Simd {
    uint64_t low, high;
} FC_Simd;

// Supplies saved x86 registers to instruction hooks; modifiers may change the resumed machine state.
typedef struct FC_CpuContext32 {
    FC_Simd xmm[8];
    uint32_t eflags, edi, esi, ebp, esp, ebx, edx, ecx, eax, eip;
} FC_CpuContext32;

// Supplies saved x64 registers to instruction hooks; modifiers may change the resumed machine state.
typedef struct FC_CpuContext64 {
    FC_Simd xmm[16];
    uint64_t rflags, r15, r14, r13, r12, r11, r10, r9, r8, rdi, rsi, rbp, rsp, rbx, rdx, rcx, rax, rip;
} FC_CpuContext64;
#if defined(_WIN64)
typedef FC_CpuContext64 FC_CpuContext;
#else
typedef FC_CpuContext32 FC_CpuContext;
#endif
// Selects a planning snapshot, prepared memory handle, expected-fault handle or explicit plan failure.
enum FC_ReadMode {
    FC_READ_SNAPSHOT,
    FC_READ_MEMORY,
    FC_READ_FAULT,
    FC_READ_FAILURE
};

// Selects the exact access-violation kind that an expected-fault handle may recover.
enum FC_NativeException {
    FC_FAULT_READ = 1,
    FC_FAULT_WRITE = 2,
    FC_FAULT_EXECUTE = 3
};

// Reserves a native range and optional evidence for a snapshot or retained resource.
typedef struct FC_ReadClaim {
    FC_Location location;
    FC_Evidence evidence;
    // Byte extent of the range; alignment is a power-of-two byte alignment.
    uint64_t size;
    uint32_t alignment, writable, mode, fault_kind;
    // MEMORY may refer to this plan's allocation instead of an image location.
    void *allocation;
    // FAILURE records this message as the plan's first error.
    FC_Text failure;
} FC_ReadClaim;

// Declares replacement bytes or NOPs to install after preimage and instruction-boundary validation.
typedef struct FC_ByteWrite {
    FC_Location location;
    FC_Evidence evidence;
    FC_Bytes bytes;
    // instructions requests complete instruction boundaries; nop fills instruction_bytes with 0x90.
    uint32_t alignment, instructions, nop;
    uint64_t instruction_bytes;
} FC_ByteWrite;

// Changes one decoded explicit operand's immediate or displacement field while retaining the instruction.
typedef struct FC_OperandEdit {
    FC_Location location;
    FC_Evidence evidence;
    // operand is zero-based; signed_value interprets value, while address_value selects address instead.
    uint32_t operand, signed_value, address_value;
    uint64_t value;
    FC_Address address;
} FC_OperandEdit;

// Reserves fixed replacement data whose storage becomes available during Prepare.
typedef struct FC_Allocation {
    // Number of elements; element_size and power-of-two alignment are in bytes.
    uint64_t count;
    uint32_t element_size, alignment;
    // Optional selected profile ID requesting allocation within relative-address reach of that image.
    FC_Text near_image;
    // Empty means zero initialization; otherwise bytes must cover count * element_size exactly.
    FC_Bytes initial;
} FC_Allocation;

// Writes an absolute pointer-sized address or a four-byte displacement relative to the end of its field.
typedef struct FC_AddressFixup {
    FC_Location location;
    FC_Evidence evidence;
    FC_Address target;
    // relative selects a signed four-byte field; absolute width must equal sizeof(void *).
    uint32_t relative, width;
} FC_AddressFixup;

// Selects a direct relative CALL or unconditional JMP replacement.
enum FC_BranchKind {
    FC_BRANCH_CALL,
    FC_BRANCH_JUMP
};

// Redirects an existing five-byte branch or inserts a branch across an exact complete-instruction range.
typedef struct FC_BranchEdit {
    FC_Location location;
    FC_Evidence evidence;
    FC_Address target;
    uint32_t kind, insert;
    // Insertion range in bytes, at least five; trailing bytes become NOPs.
    uint64_t instruction_bytes;
    // Optional signature for a retained original-call handle; available only for redirection.
    const FC_NativeCall *original_call;
} FC_BranchEdit;

// Selects a function entry, a direct CALL instruction or an instruction-context hook.
enum FC_HookScope {
    FC_HOOK_FUNCTION,
    FC_HOOK_CALL,
    FC_HOOK_INSTRUCTION
};

// Retains one modifier or before/after observers at a shared hook site; callbacks execute on the reaching thread.
typedef struct FC_HookParticipant {
    FC_Location location;
    FC_Evidence evidence;
    uint32_t scope;
    // Required logical call description for function/CALL hooks; instruction hooks use the CPU context instead.
    const FC_NativeCall *call;
    // Borrowed callback object must survive every installed invocation.
    void *context;
    // A modifier replaces a typed call or edits instruction-hook CPU state; observers surround that operation.
    // Typed observers receive const logical arguments and, after a non-void call, its const result.
    FC_NativeThunk modifier, before, after;
    // State is separately aligned and initialized per invocation, then released after callbacks without destruction.
    FC_StateInitialize initialize;
    uint64_t state_size;
    uint32_t state_alignment;
} FC_HookParticipant;

// Reserves a callable entry and signature for a handle that becomes usable after preparation.
typedef struct FC_CallOperation {
    FC_Location location;
    FC_Evidence evidence;
    FC_NativeCall call;
} FC_CallOperation;

// Selects the active payload in FC_Operation.
enum FC_OperationKind {
    FC_OPERATION_READ,
    FC_OPERATION_WRITE,
    FC_OPERATION_OPERAND,
    FC_OPERATION_ALLOCATE,
    FC_OPERATION_FIXUP,
    FC_OPERATION_BRANCH,
    FC_OPERATION_HOOK,
    FC_OPERATION_CALL
};

// Fixed payload records are selected by kind; submission copies every retained view before returning.
typedef struct FC_Operation {
    uint32_t kind;

    union {
        FC_ReadClaim read;
        FC_ByteWrite write;
        FC_OperandEdit operand;
        FC_Allocation allocation;
        FC_AddressFixup fixup;
        FC_BranchEdit branch;
        FC_HookParticipant hook;
        FC_CallOperation call;
    } value;
} FC_Operation;

// Maps a patch requirement label to the selected build's profile ID for the planning callback.
typedef struct FC_PlanImage {
    FC_Text name, profile;
} FC_PlanImage;

// Invoked synchronously on the calling thread; callback/context are borrowed and the callback must not throw.
typedef void(FC_CDECL *FC_ExpectedFaultCallback)(void *);

// Accesses stable prepared resources; check byte_size before reading a function pointer and retain the owning patch.
// data through invoke return 1 on success or 0 with a synchronous diagnostic when a sink is supplied.
typedef struct FC_NativeApi {
    // Byte extent of this table, allowing older runtimes to omit later services.
    uint32_t byte_size;
    // Returns mutable allocation storage and element count only before the owning patch exposes its resources.
    uint32_t(FC_CDECL *data)(void *, void **, uint64_t *, FC_ErrorSink);
    // Copies size bytes at a byte offset; write selects direction, and runtime range/permission checks may fail.
    uint32_t(FC_CDECL *memory)(void *, uint64_t, void *, uint64_t, uint32_t write, FC_ErrorSink);
    // Returns a borrowed memory address and byte extent after checking its current access permissions.
    uint32_t(FC_CDECL *view)(void *, void **, uint64_t *, FC_ErrorSink);
    // Resolves a prepared call or allocation resource to an absolute process address.
    uint32_t(FC_CDECL *resolve)(void *, uintptr_t *, FC_ErrorSink);
    // Executes a prepared custom call synchronously using caller-owned logical argument and result storage.
    uint32_t(FC_CDECL *invoke)(void *, const FC_NativeFrame *, FC_ErrorSink);
    // Returns 0 unavailable, 1 completed, 2 recovered at this exact instruction/access kind on the active thread.
    // Recovery may skip callback-local destructors; keep required cleanup outside the recovered scope.
    uint32_t(FC_CDECL *expected_fault)(void *, FC_ExpectedFaultCallback, void *);
    // Returns this thread's original typed-hook return address; zero outside a typed invocation or in an instruction callback.
    uintptr_t(FC_CDECL *return_address)(void);
} FC_NativeApi;

// Planning borrows the caller's plan for the callback; retained requests are copied by submit.
typedef struct FC_Plan {
    // Borrowed runtime plan identity passed unchanged to submit and read.
    void *context;
    // Copies a request and optionally returns an opaque resource; failure records the plan's first error.
    // Returns 1 on success or 0 with a synchronous diagnostic when a sink is supplied.
    uint32_t(FC_CDECL *submit)(void *, const FC_Operation *, void **, FC_ErrorSink);
    // Copies a snapshot and records the borrowed request once, including alignment and evidence.
    uint32_t(FC_CDECL *read)(void *, const FC_ReadClaim *, void *, FC_ErrorSink);
    // Borrowed selected-image bindings remain valid throughout the planning callback.
    const FC_PlanImage *images;
    uint32_t image_count;
} FC_Plan;

#ifdef __cplusplus
}
#endif
