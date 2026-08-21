#pragma once

#include <FusionCutter/Abi.h>

#include <stddef.h>

#define FC_PLUGIN_ABI_GENERATION UINT32_C(1)
#define FC_SDK_REVISION UINT32_C(1)

// Opaque framework-owned handles may be stored only for the lifetimes stated by their creating APIs.
typedef struct FC_ReportTokenTag* FC_ReportToken;
typedef struct FC_TraceHandleTag* FC_TraceHandle;
// Opaque plugin-owned instance returned by the Create callback and passed back until the Destroy callback.
typedef void* FC_PatchHandle;

// Callback status and sink submission status deliberately describe different failure boundaries.
typedef uint32_t FC_CallStatus;
#define FC_CALL_OK UINT32_C(0)
#define FC_CALL_FAILED UINT32_C(1)

typedef uint32_t FC_SubmitResult;
#define FC_SUBMIT_ACCEPTED UINT32_C(0)
#define FC_SUBMIT_REJECTED UINT32_C(1)

// Log levels let the host filter cheaply before a plugin formats or submits diagnostic text.
typedef uint32_t FC_LogLevel;
#define FC_LOG_OFF UINT32_C(0)
#define FC_LOG_ERROR UINT32_C(1)
#define FC_LOG_WARNING UINT32_C(2)
#define FC_LOG_INFO UINT32_C(3)
#define FC_LOG_DEBUG UINT32_C(4)

// Trace creation distinguishes an intentional disabled channel from a malformed or unavailable request.
typedef uint32_t FC_TraceCreateResult;
#define FC_TRACE_CREATED UINT32_C(0)
#define FC_TRACE_DISABLED UINT32_C(1)
#define FC_TRACE_REJECTED UINT32_C(2)

// Data handles identify allocations submitted during the Plan callback before the Prepare context exposes addresses.
typedef uint32_t FC_DataHandle;
#define FC_INVALID_DATA_HANDLE UINT32_C(0)

// Native call metadata describes logical values independently from their physical register or stack homes.
typedef uint32_t FC_NativeValueKind;
#define FC_NATIVE_VOID UINT32_C(0)
#define FC_NATIVE_INTEGER UINT32_C(1)
#define FC_NATIVE_POINTER UINT32_C(2)
#define FC_NATIVE_FLOAT_32 UINT32_C(3)
#define FC_NATIVE_FLOAT_64 UINT32_C(4)
#define FC_NATIVE_RECORD UINT32_C(5)

typedef uint32_t FC_NativeStorageKind;
#define FC_NATIVE_STORAGE_NONE UINT32_C(0)
#define FC_NATIVE_STORAGE_REGISTER UINT32_C(1)
#define FC_NATIVE_STORAGE_STACK UINT32_C(2)

// Register IDs are stable ABI values shared by call descriptions and hook contexts.
typedef uint32_t FC_NativeRegister;
#define FC_REGISTER_NONE UINT32_C(0)
#define FC_REGISTER_EAX UINT32_C(1)
#define FC_REGISTER_EBX UINT32_C(2)
#define FC_REGISTER_ECX UINT32_C(3)
#define FC_REGISTER_EDX UINT32_C(4)
#define FC_REGISTER_ESI UINT32_C(5)
#define FC_REGISTER_EDI UINT32_C(6)
#define FC_REGISTER_EBP UINT32_C(7)
#define FC_REGISTER_ST0 UINT32_C(8)
#define FC_REGISTER_RAX UINT32_C(16)
#define FC_REGISTER_RBX UINT32_C(17)
#define FC_REGISTER_RCX UINT32_C(18)
#define FC_REGISTER_RDX UINT32_C(19)
#define FC_REGISTER_RSI UINT32_C(20)
#define FC_REGISTER_RDI UINT32_C(21)
#define FC_REGISTER_RBP UINT32_C(22)
#define FC_REGISTER_R8 UINT32_C(23)
#define FC_REGISTER_R9 UINT32_C(24)
#define FC_REGISTER_R10 UINT32_C(25)
#define FC_REGISTER_R11 UINT32_C(26)
#define FC_REGISTER_R12 UINT32_C(27)
#define FC_REGISTER_R13 UINT32_C(28)
#define FC_REGISTER_R14 UINT32_C(29)
#define FC_REGISTER_R15 UINT32_C(30)
#define FC_REGISTER_XMM0 UINT32_C(32)
#define FC_REGISTER_XMM1 UINT32_C(33)
#define FC_REGISTER_XMM2 UINT32_C(34)
#define FC_REGISTER_XMM3 UINT32_C(35)
#define FC_REGISTER_XMM4 UINT32_C(36)
#define FC_REGISTER_XMM5 UINT32_C(37)
#define FC_REGISTER_XMM6 UINT32_C(38)
#define FC_REGISTER_XMM7 UINT32_C(39)
#define FC_REGISTER_XMM8 UINT32_C(40)
#define FC_REGISTER_XMM9 UINT32_C(41)
#define FC_REGISTER_XMM10 UINT32_C(42)
#define FC_REGISTER_XMM11 UINT32_C(43)
#define FC_REGISTER_XMM12 UINT32_C(44)
#define FC_REGISTER_XMM13 UINT32_C(45)
#define FC_REGISTER_XMM14 UINT32_C(46)
#define FC_REGISTER_XMM15 UINT32_C(47)

// Stack cleanup states who restores an x86 call frame; x64 normalized calls use NONE.
typedef uint32_t FC_StackCleanup;
#define FC_STACK_CLEANUP_NONE UINT32_C(0)
#define FC_STACK_CLEANUP_CALLER UINT32_C(1)
#define FC_STACK_CLEANUP_CALLEE UINT32_C(2)

// Setting kinds select the active FC_SettingValue member and the validation rules applied by the framework.
typedef uint32_t FC_SettingType;
#define FC_SETTING_BOOLEAN UINT32_C(1)
#define FC_SETTING_SIGNED_8 UINT32_C(2)
#define FC_SETTING_SIGNED_16 UINT32_C(3)
#define FC_SETTING_SIGNED_32 UINT32_C(4)
#define FC_SETTING_SIGNED_64 UINT32_C(5)
#define FC_SETTING_UNSIGNED_8 UINT32_C(6)
#define FC_SETTING_UNSIGNED_16 UINT32_C(7)
#define FC_SETTING_UNSIGNED_32 UINT32_C(8)
#define FC_SETTING_UNSIGNED_64 UINT32_C(9)
#define FC_SETTING_FLOAT_32 UINT32_C(10)
#define FC_SETTING_FLOAT_64 UINT32_C(11)
#define FC_SETTING_STRING UINT32_C(12)
#define FC_SETTING_CHOICE UINT32_C(13)

// Patch plan vocabulary separates validation evidence, semantic locations, address targets, and requested operations.
typedef uint32_t FC_EvidenceKind;
#define FC_EVIDENCE_NONE UINT32_C(0)
#define FC_EVIDENCE_EXACT_BYTES UINT32_C(1)
#define FC_EVIDENCE_MASKED_BYTES UINT32_C(2)
#define FC_EVIDENCE_POINTS_TO UINT32_C(3)
#define FC_EVIDENCE_DIRECT_CALL_TO UINT32_C(4)
#define FC_EVIDENCE_DIRECT_JUMP_TO UINT32_C(5)

typedef uint32_t FC_LocationKind;
#define FC_LOCATION_DATA UINT32_C(1)
#define FC_LOCATION_FUNCTION UINT32_C(2)
#define FC_LOCATION_CODE UINT32_C(3)

typedef uint32_t FC_AddressTargetKind;
#define FC_ADDRESS_IMAGE UINT32_C(1)
#define FC_ADDRESS_DATA UINT32_C(2)
#define FC_ADDRESS_PLUGIN_FUNCTION UINT32_C(3)

typedef uint32_t FC_WriteKind;
#define FC_WRITE_BYTES UINT32_C(1)
#define FC_WRITE_POINTER UINT32_C(2)
#define FC_WRITE_REL32 UINT32_C(3)
#define FC_WRITE_CALL UINT32_C(4)
#define FC_WRITE_JUMP UINT32_C(5)

typedef uint32_t FC_RedirectKind;
#define FC_REDIRECT_CALL UINT32_C(1)
#define FC_REDIRECT_JUMP UINT32_C(2)

typedef uint32_t FC_HookKind;
#define FC_HOOK_FUNCTION_ENTRY UINT32_C(1)
#define FC_HOOK_DIRECT_CALL_SITE UINT32_C(2)
#define FC_HOOK_INSTRUCTION UINT32_C(3)

#define FC_HOOK_ENTRY_MAX_SIZE UINT32_C(4096)

typedef struct FC_ErrorSink FC_ErrorSink;
typedef struct FC_RegistrySink FC_RegistrySink;
typedef struct FC_HostApi FC_HostApi;
typedef struct FC_PluginApi FC_PluginApi;
typedef struct FC_PluginDefinition FC_PluginDefinition;
typedef struct FC_CategoryDefinition FC_CategoryDefinition;
typedef struct FC_GroupDefinition FC_GroupDefinition;
typedef struct FC_PatchDefinition FC_PatchDefinition;
typedef struct FC_SupportDefinition FC_SupportDefinition;
typedef struct FC_SettingDefinition FC_SettingDefinition;
typedef struct FC_SettingsView FC_SettingsView;
typedef struct FC_CreateContext FC_CreateContext;
typedef struct FC_PlanContext FC_PlanContext;
typedef struct FC_PrepareContext FC_PrepareContext;
typedef struct FC_ActivateContext FC_ActivateContext;
typedef struct FC_UpdateContext FC_UpdateContext;
typedef struct FC_PlanSink FC_PlanSink;
typedef struct FC_StatusSink FC_StatusSink;
typedef struct FC_TraceDefinition FC_TraceDefinition;
typedef struct FC_NativeCall FC_NativeCall;
typedef struct FC_InterfaceBindingRequest FC_InterfaceBindingRequest;
typedef struct FC_HookRequest FC_HookRequest;
typedef struct FC_ObserverRequest FC_ObserverRequest;

#pragma pack(push, 8)

// Snapshot written into plugin-owned storage by the host callback; counters are monotonic for one trace handle.
typedef struct FC_TraceHealth {
    uint32_t struct_size;
    uint64_t accepted;
    uint64_t written;
    uint64_t dropped;
    FC_Bool file_limit_reached;
    FC_Bool output_failed;
} FC_TraceHealth;

// Logging and tracing callbacks are safe only with the context and opaque handles supplied by the same host table.
typedef FC_Bool(FC_CALL* FC_LogEnabledFn)(void* context, FC_ReportToken report, FC_LogLevel level);
typedef void(FC_CALL* FC_LogWriteFn)(void* context, FC_ReportToken report, FC_LogLevel level, FC_StringView message);
typedef FC_Bool(FC_CALL* FC_TraceEnabledFn)(void* context, FC_TraceHandle trace);
typedef FC_Bool(FC_CALL* FC_TraceTryWriteFn)(void* context, FC_TraceHandle trace, FC_ByteView record);
typedef void(FC_CALL* FC_TraceHealthFn)(void* context, FC_TraceHandle trace, FC_TraceHealth* output);

// Process-lifetime framework services. Callers pass the stored context back unchanged.
struct FC_HostApi {
    uint32_t struct_size;
    void* context;
    FC_LogEnabledFn log_enabled;
    FC_LogWriteFn log_write;
    FC_TraceEnabledFn trace_enabled;
    FC_TraceTryWriteFn trace_try_write;
    FC_TraceHealthFn trace_health;
};

// Callback-scoped framework error output. set copies both views before it returns.
struct FC_ErrorSink {
    uint32_t struct_size;
    void* context;
    void(FC_CALL* set)(void* context, FC_StringView message, FC_StringView operation);
};

// Target facts and phase contexts are borrowed for one callback; image_profile uses framework-owned catalog storage.
typedef struct FC_TargetInfo {
    FC_TargetLayout layout;
    FC_HostRole role;
    FC_Architecture architecture;
    FC_StringView image_profile;
} FC_TargetInfo;

struct FC_CreateContext {
    uint32_t struct_size;
    FC_ReportToken report;
    FC_TargetInfo target;
};

struct FC_PlanContext {
    uint32_t struct_size;
    FC_ReportToken report;
    FC_TargetInfo target;
};

// Exact native calling convention and storage description copied with the containing Plan callback submission.
typedef struct FC_NativeValue {
    FC_NativeValueKind kind;
    uint32_t size;
    uint32_t alignment;
} FC_NativeValue;

typedef struct FC_NativeStorage {
    FC_NativeStorageKind kind;
    FC_NativeRegister register_id;
    uint32_t stack_offset;
} FC_NativeStorage;

typedef struct FC_NativeArgument {
    FC_NativeValue value;
    FC_NativeStorage storage;
} FC_NativeArgument;

struct FC_NativeCall {
    uint32_t struct_size;
    FC_NativeValue result;
    FC_NativeStorage return_storage;
    const FC_NativeArgument* arguments;
    uint32_t argument_count;
    FC_StackCleanup cleanup;
    uint32_t stack_size;
};

// The setting type determines the active member; string views are borrowed under the surrounding call contract.
typedef union FC_SettingValue {
    FC_Bool boolean_value;
    int64_t signed_value;
    uint64_t unsigned_value;
    double floating_value;
    FC_StringView string_value;
    uint32_t choice_index;
} FC_SettingValue;

// Setting metadata copied during registration. All pointer/count views are copied during registry submission.
struct FC_SettingDefinition {
    FC_StringView section;
    FC_StringView key;
    FC_StringView description;
    FC_StringView environment;
    FC_SettingType type;
    FC_Bool has_range;
    FC_SettingValue default_value;
    FC_SettingValue minimum;
    FC_SettingValue maximum;
    uint32_t max_length;
    const FC_StringView* choices;
    uint32_t choice_count;
};

// The Create callback receives resolved values in its patch's effective schema order; the view expires on return.
struct FC_SettingsView {
    uint32_t struct_size;
    const FC_SettingValue* values;
    uint32_t count;
};

// Evidence and semantic locations are borrowed inputs to the Plan callback; the framework copies them synchronously.
typedef struct FC_Evidence {
    FC_EvidenceKind kind;
    FC_ByteView bytes;
    FC_ByteView mask;
    uint32_t target_rva;
} FC_Evidence;

typedef struct FC_LocationView {
    FC_LocationKind kind;
    uint32_t rva;
    FC_StringView name;
    FC_StringView label;
    FC_Evidence evidence;
} FC_LocationView;

typedef struct FC_AddressTarget {
    FC_AddressTargetKind kind;
    uint32_t image_rva;
    FC_DataHandle data;
    uint64_t data_offset;
    uintptr_t plugin_function;
} FC_AddressTarget;

// Bit-preserving SIMD storage offers typed views without imposing one interpretation on instruction hooks.
typedef union FC_SimdRegister {
    uint8_t u8[16];
    uint16_t u16[8];
    uint32_t u32[4];
    uint64_t u64[2];
    float f32[4];
    double f64[2];
} FC_SimdRegister;

// Invocation-scoped register state for instruction hooks; resume_* is the explicit continuation stack pointer.
#if defined(_M_IX86)
typedef struct FC_CpuContext {
    FC_SimdRegister xmm0, xmm1, xmm2, xmm3;
    FC_SimdRegister xmm4, xmm5, xmm6, xmm7;
    uint32_t eflags;
    uint32_t edi, esi, edx, ecx, ebx, eax, ebp;
    uint32_t esp;
    uint32_t resume_esp;
    uint32_t eip;
} FC_CpuContext;
#elif defined(_M_X64)
typedef struct FC_CpuContext {
    FC_SimdRegister xmm0, xmm1, xmm2, xmm3;
    FC_SimdRegister xmm4, xmm5, xmm6, xmm7;
    FC_SimdRegister xmm8, xmm9, xmm10, xmm11;
    FC_SimdRegister xmm12, xmm13, xmm14, xmm15;
    uint64_t rflags;
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rdi, rsi, rdx, rcx, rbx, rax, rbp;
    uint64_t rsp;
    uint64_t resume_rsp;
    uint64_t rip;
} FC_CpuContext;
#else
#error Fusion Cutter supports only x86 and x64 plugin ABIs.
#endif

// Snapshots of installed state expose immutable owner and observer dispatch metadata to generated hook entries.
typedef struct FC_HookOwnerEntry {
    void* context;
    uintptr_t callback;
} FC_HookOwnerEntry;

typedef struct FC_HookObserverEntry {
    void* context;
    uintptr_t before;
    uintptr_t after;
    uint32_t state_offset;
    uint32_t state_size;
} FC_HookObserverEntry;

typedef struct FC_HookSnapshot {
    uint32_t struct_size;
    uintptr_t original;
    FC_HookOwnerEntry owner;
    const FC_HookObserverEntry* observers;
    uint32_t observer_count;
    uint32_t total_state_size;
} FC_HookSnapshot;

typedef void(FC_CALL* FC_BindOriginalFn)(void* context, uintptr_t original);

// Valid only for the build call; the builder initializes the complete supplied entry extent.
typedef struct FC_HookBuildInput {
    uint32_t struct_size;
    uint8_t* entry;
    uint32_t entry_size;
    const void* snapshot_slot;
} FC_HookBuildInput;

typedef FC_CallStatus(FC_CALL* FC_BuildHookFn)(const FC_HookBuildInput* input, const FC_ErrorSink* error);

typedef struct FC_HookBuilder {
    FC_BuildHookFn build;
    uint32_t entry_size;
} FC_HookBuilder;

// A hook owner may modify execution and bind the callable original; the sink copies its submitted plan data.
struct FC_HookRequest {
    uint32_t struct_size;
    FC_LocationView location;
    FC_HookKind kind;
    const FC_NativeCall* native_call;
    FC_HookBuilder builder;
    void* context;
    uintptr_t callback;
    void* original_context;
    FC_BindOriginalFn bind_original;
};

// An observer cannot replace the owner and may reserve invocation-local state shared by its before/after callbacks.
struct FC_ObserverRequest {
    uint32_t struct_size;
    FC_LocationView location;
    FC_HookKind kind;
    const FC_NativeCall* native_call;
    FC_HookBuilder builder;
    void* context;
    uintptr_t before;
    uintptr_t after;
    uint32_t state_size;
    uint32_t state_alignment;
};

typedef void(FC_CALL* FC_InterfaceConnectFn)(void* context, const void* value);

// Optional interface connection delivered after activation with the request's plugin-owned context.
struct FC_InterfaceBindingRequest {
    uint32_t struct_size;
    FC_StringView provider_patch;
    FC_StringView id;
    uint32_t size;
    void* context;
    FC_InterfaceConnectFn connect;
};

// Operation requests are borrowed for one sink call and become owned only when that call accepts them.
typedef struct FC_RequireRequest {
    uint32_t struct_size;
    FC_LocationView location;
    uint64_t size;
    uint32_t alignment;
    FC_Bool writable;
    const FC_NativeCall* native_call;
} FC_RequireRequest;

typedef struct FC_WriteRequest {
    uint32_t struct_size;
    FC_LocationView location;
    FC_WriteKind kind;
    FC_ByteView bytes;
    FC_AddressTarget target;
} FC_WriteRequest;

typedef struct FC_NopRequest {
    uint32_t struct_size;
    FC_LocationView location;
    uint64_t size;
} FC_NopRequest;

typedef struct FC_RedirectRequest {
    uint32_t struct_size;
    FC_LocationView location;
    FC_RedirectKind kind;
    FC_AddressTarget target;
} FC_RedirectRequest;

typedef struct FC_DataAllocationRequest {
    uint32_t struct_size;
    uint64_t byte_size;
    uint32_t alignment;
    FC_ByteView initial_bytes;
    FC_StringView name;
} FC_DataAllocationRequest;

// Framework-owned transport for one Plan callback. Every accepted request is copied before its operation returns.
struct FC_PlanSink {
    uint32_t struct_size;
    void* context;
    FC_SubmitResult(FC_CALL* require)(void* context, const FC_RequireRequest* request, uintptr_t* resolved_address);
    FC_SubmitResult(FC_CALL* write)(void* context, const FC_WriteRequest* request);
    FC_SubmitResult(FC_CALL* nop)(void* context, const FC_NopRequest* request);
    FC_SubmitResult(FC_CALL* redirect)(void* context, const FC_RedirectRequest* request, uintptr_t* original_target);
    FC_SubmitResult(FC_CALL* allocate_data)(void* context, const FC_DataAllocationRequest* request,
                                            FC_DataHandle* output);
    FC_SubmitResult(FC_CALL* bind_interface)(void* context, const FC_InterfaceBindingRequest* request);
    FC_SubmitResult(FC_CALL* hook)(void* context, const FC_HookRequest* request);
    FC_SubmitResult(FC_CALL* observe)(void* context, const FC_ObserverRequest* request);
};

// Callback-scoped bounded status output; every accepted label and text value is copied during its call.
struct FC_StatusSink {
    uint32_t struct_size;
    void* context;
    FC_Bool(FC_CALL* add_text)(void* context, FC_StringView label, FC_StringView value);
    FC_Bool(FC_CALL* add_signed)(void* context, FC_StringView label, int64_t value);
    FC_Bool(FC_CALL* add_unsigned)(void* context, FC_StringView label, uint64_t value);
    FC_Bool(FC_CALL* add_floating)(void* context, FC_StringView label, double value);
    FC_Bool(FC_CALL* add_boolean)(void* context, FC_StringView label, FC_Bool value);
};

// Consumed synchronously when created; returned trace handles remain framework-owned.
struct FC_TraceDefinition {
    uint32_t struct_size;
    FC_StringView name;
    uint32_t capacity;
    uint32_t max_record_size;
    uint32_t version;
};

typedef FC_Bool(FC_CALL* FC_ResolveDataFn)(void* context, FC_DataHandle data, uintptr_t* address, uint64_t* byte_size);
typedef FC_TraceCreateResult(FC_CALL* FC_CreateTraceFn)(void* context, const FC_TraceDefinition* definition,
                                                        FC_TraceHandle* output);
typedef FC_Bool(FC_CALL* FC_FindInterfaceFn)(void* context, FC_StringView provider_patch, FC_StringView id,
                                             uint32_t size, void* output);

// Prepare capabilities resolve only resources and interfaces admitted by the patch plan and selection result.
struct FC_PrepareContext {
    uint32_t struct_size;
    FC_ReportToken report;
    void* context;
    FC_ResolveDataFn resolve_data;
    FC_CreateTraceFn create_trace;
    FC_FindInterfaceFn find_interface;
};

// The Activate and Update callbacks expose reporting only; native mutation and fallible setup have already ended.
struct FC_ActivateContext {
    uint32_t struct_size;
    FC_ReportToken report;
};

struct FC_UpdateContext {
    uint32_t struct_size;
    FC_ReportToken report;
};

// Plugin-owned lifecycle operations. Capabilities in each phase context expire when their callback returns.
typedef FC_CallStatus(FC_CALL* FC_PatchCreateFn)(void* context, const FC_CreateContext* create,
                                                 const FC_SettingsView* settings, const FC_ErrorSink* error,
                                                 FC_PatchHandle* output);
typedef void(FC_CALL* FC_PatchDestroyFn)(void* context, FC_PatchHandle patch);
typedef FC_CallStatus(FC_CALL* FC_PatchPlanFn)(void* context, FC_PatchHandle patch, const FC_PlanContext* plan,
                                               const FC_PlanSink* sink, const FC_ErrorSink* error);
typedef FC_CallStatus(FC_CALL* FC_PatchPrepareFn)(void* context, FC_PatchHandle patch, const FC_PrepareContext* prepare,
                                                  const FC_ErrorSink* error);
typedef void(FC_CALL* FC_PatchActivateFn)(void* context, FC_PatchHandle patch, const FC_ActivateContext* activate);
typedef void(FC_CALL* FC_PatchUpdateFn)(void* context, FC_PatchHandle patch, const FC_UpdateContext* update);
typedef void(FC_CALL* FC_PatchWriteStatusFn)(void* context, FC_PatchHandle patch, const FC_StatusSink* status);
typedef FC_Bool(FC_CALL* FC_PatchQueryInterfaceFn)(void* context, FC_PatchHandle patch, FC_StringView id, uint32_t size,
                                                   void* output);

// The source contribution owns its context and callback code until the admitted registration state is released.
typedef struct FC_PatchCallbacks {
    void* context;
    FC_PatchCreateFn create;
    FC_PatchDestroyFn destroy;
    FC_PatchPlanFn plan;
    FC_PatchPrepareFn prepare;
    FC_PatchActivateFn activate;
    FC_PatchUpdateFn update;
    FC_PatchWriteStatusFn write_status;
    FC_PatchQueryInterfaceFn query_interface;
} FC_PatchCallbacks;

// Borrowed fixed-depth registration tree copied completely by the registry submission callback.
struct FC_CategoryDefinition {
    FC_StringView id;
    FC_Bool has_order;
    uint32_t order;
};

struct FC_GroupDefinition {
    FC_StringView id;
    const FC_StringView* members;
    uint32_t member_count;
    FC_Bool configurable;
    FC_Bool enabled;
    FC_StringView category;
    FC_StringView description;
};

struct FC_SupportDefinition {
    FC_TargetLayout layouts;
    FC_HostRole roles;
    FC_TargetImage image;
    FC_PatchCallbacks callbacks;
    FC_Bool has_settings;
    const FC_SettingDefinition* settings;
    uint32_t setting_count;
    const FC_StringView* depends_on;
    uint32_t depends_on_count;
    const FC_StringView* includes;
    uint32_t include_count;
    FC_FailurePolicy failure_policy;
};

struct FC_PatchDefinition {
    FC_StringView id;
    FC_StringView name;
    FC_StringView description;
    FC_StringView version;
    FC_StringView author;
    FC_StringView source;
    FC_Bool configurable;
    FC_Bool enabled;
    FC_StringView category;
    FC_FailurePolicy failure_policy;
    const FC_SettingDefinition* settings;
    uint32_t setting_count;
    const FC_StringView* depends_on;
    uint32_t depends_on_count;
    const FC_StringView* includes;
    uint32_t include_count;
    const FC_SupportDefinition* supports;
    uint32_t support_count;
};

struct FC_PluginDefinition {
    uint32_t struct_size;
    FC_StringView id;
    FC_StringView version;
    FC_StringView author;
    FC_StringView source;
    const FC_CategoryDefinition* categories;
    uint32_t category_count;
    const FC_GroupDefinition* groups;
    uint32_t group_count;
    const FC_PatchDefinition* patches;
    uint32_t patch_count;
};

// Accepts exactly one definition per registration callback and completes its deep copy synchronously.
struct FC_RegistrySink {
    uint32_t struct_size;
    void* context;
    FC_SubmitResult(FC_CALL* submit)(void* context, const FC_PluginDefinition* plugin);
};

// Registration executes once per acquisition and must complete all registry submission synchronously.
typedef FC_CallStatus(FC_CALL* FC_RegisterPluginFn)(const FC_HostApi* host, const FC_RegistrySink* registry,
                                                    const FC_ErrorSink* error);

// Immutable DLL-owned query table; the framework copies its known prefix during plugin admission.
struct FC_PluginApi {
    uint32_t struct_size;
    uint32_t sdk_revision;
    uint32_t host_api_size;
    FC_RegisterPluginFn register_plugin;
};

#pragma pack(pop)

// Returns the immutable table for a supported generation without performing registration or other runtime work.
FC_EXTERN_C const FC_PluginApi* FC_CALL FusionCutter_QueryPlugin(uint32_t abi_generation) FC_NOEXCEPT;

#if defined(__cplusplus)
#define FC_ABI_STATIC_ASSERT(expression, message) static_assert((expression), message)
#define FC_ABI_ALIGNOF(type) alignof(type)
#else
#define FC_ABI_STATIC_ASSERT(expression, message) _Static_assert((expression), message)
#define FC_ABI_ALIGNOF(type) _Alignof(type)
#endif

FC_ABI_STATIC_ASSERT(sizeof(FC_TraceHealth) == 40, "FC_TraceHealth layout changed");
FC_ABI_STATIC_ASSERT(FC_ABI_ALIGNOF(FC_TraceHealth) == 8, "FC_TraceHealth alignment changed");
FC_ABI_STATIC_ASSERT(sizeof(FC_SimdRegister) == 16, "FC_SimdRegister layout changed");
FC_ABI_STATIC_ASSERT(FC_ABI_ALIGNOF(FC_SimdRegister) == 8, "FC_SimdRegister alignment changed");

#if defined(_M_IX86)
FC_ABI_STATIC_ASSERT(sizeof(FC_HostApi) == 28, "x86 FC_HostApi layout changed");
FC_ABI_STATIC_ASSERT(FC_ABI_ALIGNOF(FC_HostApi) == 4, "x86 FC_HostApi alignment changed");
FC_ABI_STATIC_ASSERT(sizeof(FC_TargetInfo) == 20, "x86 FC_TargetInfo layout changed");
FC_ABI_STATIC_ASSERT(sizeof(FC_SettingsView) == 12, "x86 FC_SettingsView layout changed");
FC_ABI_STATIC_ASSERT(sizeof(FC_SettingDefinition) == 80, "x86 FC_SettingDefinition layout changed");
FC_ABI_STATIC_ASSERT(sizeof(FC_PatchCallbacks) == 36, "x86 FC_PatchCallbacks layout changed");
FC_ABI_STATIC_ASSERT(sizeof(FC_CategoryDefinition) == 16, "x86 FC_CategoryDefinition layout changed");
FC_ABI_STATIC_ASSERT(sizeof(FC_GroupDefinition) == 40, "x86 FC_GroupDefinition layout changed");
FC_ABI_STATIC_ASSERT(sizeof(FC_SupportDefinition) == 80, "x86 FC_SupportDefinition layout changed");
FC_ABI_STATIC_ASSERT(sizeof(FC_PatchDefinition) == 100, "x86 FC_PatchDefinition layout changed");
FC_ABI_STATIC_ASSERT(sizeof(FC_RegistrySink) == 12, "x86 FC_RegistrySink layout changed");
FC_ABI_STATIC_ASSERT(sizeof(FC_PluginApi) == 16, "x86 FC_PluginApi layout changed");
FC_ABI_STATIC_ASSERT(FC_ABI_ALIGNOF(FC_PluginApi) == 4, "x86 FC_PluginApi alignment changed");
FC_ABI_STATIC_ASSERT(offsetof(FC_PluginApi, register_plugin) == 12, "x86 FC_PluginApi callback offset changed");
FC_ABI_STATIC_ASSERT(sizeof(FC_CreateContext) == 28, "x86 FC_CreateContext layout changed");
FC_ABI_STATIC_ASSERT(FC_ABI_ALIGNOF(FC_CreateContext) == 4, "x86 FC_CreateContext alignment changed");
FC_ABI_STATIC_ASSERT(offsetof(FC_CreateContext, target) == 8, "x86 FC_CreateContext target offset changed");
FC_ABI_STATIC_ASSERT(sizeof(FC_PlanContext) == 28, "x86 FC_PlanContext layout changed");
FC_ABI_STATIC_ASSERT(FC_ABI_ALIGNOF(FC_PlanContext) == 4, "x86 FC_PlanContext alignment changed");
FC_ABI_STATIC_ASSERT(offsetof(FC_PlanContext, target) == 8, "x86 FC_PlanContext target offset changed");
FC_ABI_STATIC_ASSERT(sizeof(FC_PrepareContext) == 24, "x86 FC_PrepareContext layout changed");
FC_ABI_STATIC_ASSERT(FC_ABI_ALIGNOF(FC_PrepareContext) == 4, "x86 FC_PrepareContext alignment changed");
FC_ABI_STATIC_ASSERT(offsetof(FC_PrepareContext, resolve_data) == 12, "x86 FC_PrepareContext callback offset changed");
FC_ABI_STATIC_ASSERT(sizeof(FC_ActivateContext) == 8, "x86 FC_ActivateContext layout changed");
FC_ABI_STATIC_ASSERT(sizeof(FC_UpdateContext) == 8, "x86 FC_UpdateContext layout changed");
FC_ABI_STATIC_ASSERT(sizeof(FC_CpuContext) == 176, "x86 FC_CpuContext layout changed");
FC_ABI_STATIC_ASSERT(FC_ABI_ALIGNOF(FC_CpuContext) == 8, "x86 FC_CpuContext alignment changed");
FC_ABI_STATIC_ASSERT(offsetof(FC_CpuContext, eflags) == 128, "x86 FC_CpuContext flags offset changed");
FC_ABI_STATIC_ASSERT(offsetof(FC_CpuContext, resume_esp) == 164, "x86 FC_CpuContext resume offset changed");
FC_ABI_STATIC_ASSERT(sizeof(FC_HookOwnerEntry) == 8, "x86 FC_HookOwnerEntry layout changed");
FC_ABI_STATIC_ASSERT(sizeof(FC_HookObserverEntry) == 20, "x86 FC_HookObserverEntry layout changed");
FC_ABI_STATIC_ASSERT(sizeof(FC_HookSnapshot) == 28, "x86 FC_HookSnapshot layout changed");
FC_ABI_STATIC_ASSERT(offsetof(FC_HookSnapshot, original) == 4, "x86 FC_HookSnapshot original offset changed");
FC_ABI_STATIC_ASSERT(offsetof(FC_HookSnapshot, observers) == 16, "x86 FC_HookSnapshot observers offset changed");
FC_ABI_STATIC_ASSERT(sizeof(FC_PluginDefinition) == 60, "x86 FC_PluginDefinition layout changed");
FC_ABI_STATIC_ASSERT(FC_ABI_ALIGNOF(FC_PluginDefinition) == 4, "x86 FC_PluginDefinition alignment changed");
FC_ABI_STATIC_ASSERT(offsetof(FC_PluginDefinition, id) == 4, "x86 FC_PluginDefinition ID offset changed");
FC_ABI_STATIC_ASSERT(offsetof(FC_PluginDefinition, patches) == 52, "x86 FC_PluginDefinition patches offset changed");
FC_ABI_STATIC_ASSERT(offsetof(FC_GroupDefinition, members) == 8, "x86 FC_GroupDefinition members offset changed");
FC_ABI_STATIC_ASSERT(offsetof(FC_SupportDefinition, callbacks) == 12,
                     "x86 FC_SupportDefinition callbacks offset changed");
FC_ABI_STATIC_ASSERT(offsetof(FC_PatchDefinition, supports) == 92, "x86 FC_PatchDefinition supports offset changed");
FC_ABI_STATIC_ASSERT(offsetof(FC_RegistrySink, submit) == 8, "x86 FC_RegistrySink callback offset changed");
FC_ABI_STATIC_ASSERT(sizeof(FC_HookBuildInput) == 16, "x86 FC_HookBuildInput layout changed");
FC_ABI_STATIC_ASSERT(FC_ABI_ALIGNOF(FC_HookBuildInput) == 4, "x86 FC_HookBuildInput alignment changed");
FC_ABI_STATIC_ASSERT(sizeof(FC_HookBuilder) == 8, "x86 FC_HookBuilder layout changed");
FC_ABI_STATIC_ASSERT(FC_ABI_ALIGNOF(FC_HookBuilder) == 4, "x86 FC_HookBuilder alignment changed");
FC_ABI_STATIC_ASSERT(sizeof(FC_HookRequest) == 84, "x86 FC_HookRequest layout changed");
FC_ABI_STATIC_ASSERT(FC_ABI_ALIGNOF(FC_HookRequest) == 4, "x86 FC_HookRequest alignment changed");
FC_ABI_STATIC_ASSERT(sizeof(FC_ObserverRequest) == 88, "x86 FC_ObserverRequest layout changed");
FC_ABI_STATIC_ASSERT(FC_ABI_ALIGNOF(FC_ObserverRequest) == 4, "x86 FC_ObserverRequest alignment changed");
FC_ABI_STATIC_ASSERT(offsetof(FC_HookBuildInput, struct_size) == 0, "x86 hook build input prefix changed");
FC_ABI_STATIC_ASSERT(offsetof(FC_HookBuildInput, entry) == 4, "x86 hook build input entry offset changed");
FC_ABI_STATIC_ASSERT(offsetof(FC_HookBuildInput, entry_size) == 8, "x86 hook build input size offset changed");
FC_ABI_STATIC_ASSERT(offsetof(FC_HookBuildInput, snapshot_slot) == 12, "x86 hook build input offset changed");
FC_ABI_STATIC_ASSERT(offsetof(FC_HookBuilder, build) == 0, "x86 hook builder callback offset changed");
FC_ABI_STATIC_ASSERT(offsetof(FC_HookBuilder, entry_size) == 4, "x86 hook builder size offset changed");
FC_ABI_STATIC_ASSERT(offsetof(FC_HookRequest, struct_size) == 0, "x86 hook request prefix changed");
FC_ABI_STATIC_ASSERT(offsetof(FC_HookRequest, location) == 4, "x86 hook request location offset changed");
FC_ABI_STATIC_ASSERT(offsetof(FC_HookRequest, kind) == 52, "x86 hook request kind offset changed");
FC_ABI_STATIC_ASSERT(offsetof(FC_HookRequest, native_call) == 56, "x86 hook request call offset changed");
FC_ABI_STATIC_ASSERT(offsetof(FC_HookRequest, builder) == 60, "x86 hook request builder offset changed");
FC_ABI_STATIC_ASSERT(offsetof(FC_HookRequest, context) == 68, "x86 hook request context offset changed");
FC_ABI_STATIC_ASSERT(offsetof(FC_HookRequest, callback) == 72, "x86 hook request callback offset changed");
FC_ABI_STATIC_ASSERT(offsetof(FC_HookRequest, original_context) == 76,
                     "x86 hook request original context offset changed");
FC_ABI_STATIC_ASSERT(offsetof(FC_HookRequest, bind_original) == 80, "x86 hook request tail offset changed");
FC_ABI_STATIC_ASSERT(offsetof(FC_ObserverRequest, struct_size) == 0, "x86 observer request prefix changed");
FC_ABI_STATIC_ASSERT(offsetof(FC_ObserverRequest, location) == 4, "x86 observer request location offset changed");
FC_ABI_STATIC_ASSERT(offsetof(FC_ObserverRequest, kind) == 52, "x86 observer request kind offset changed");
FC_ABI_STATIC_ASSERT(offsetof(FC_ObserverRequest, native_call) == 56, "x86 observer request call offset changed");
FC_ABI_STATIC_ASSERT(offsetof(FC_ObserverRequest, builder) == 60, "x86 observer request builder offset changed");
FC_ABI_STATIC_ASSERT(offsetof(FC_ObserverRequest, context) == 68, "x86 observer request context offset changed");
FC_ABI_STATIC_ASSERT(offsetof(FC_ObserverRequest, before) == 72, "x86 observer request before offset changed");
FC_ABI_STATIC_ASSERT(offsetof(FC_ObserverRequest, after) == 76, "x86 observer request after offset changed");
FC_ABI_STATIC_ASSERT(offsetof(FC_ObserverRequest, state_size) == 80, "x86 observer request state size offset changed");
FC_ABI_STATIC_ASSERT(offsetof(FC_ObserverRequest, state_alignment) == 84, "x86 observer request tail offset changed");
#elif defined(_M_X64)
FC_ABI_STATIC_ASSERT(sizeof(FC_HostApi) == 56, "x64 FC_HostApi layout changed");
FC_ABI_STATIC_ASSERT(FC_ABI_ALIGNOF(FC_HostApi) == 8, "x64 FC_HostApi alignment changed");
FC_ABI_STATIC_ASSERT(sizeof(FC_TargetInfo) == 32, "x64 FC_TargetInfo layout changed");
FC_ABI_STATIC_ASSERT(sizeof(FC_SettingsView) == 24, "x64 FC_SettingsView layout changed");
FC_ABI_STATIC_ASSERT(sizeof(FC_SettingDefinition) == 144, "x64 FC_SettingDefinition layout changed");
FC_ABI_STATIC_ASSERT(sizeof(FC_PatchCallbacks) == 72, "x64 FC_PatchCallbacks layout changed");
FC_ABI_STATIC_ASSERT(sizeof(FC_CategoryDefinition) == 24, "x64 FC_CategoryDefinition layout changed");
FC_ABI_STATIC_ASSERT(sizeof(FC_GroupDefinition) == 72, "x64 FC_GroupDefinition layout changed");
FC_ABI_STATIC_ASSERT(sizeof(FC_SupportDefinition) == 144, "x64 FC_SupportDefinition layout changed");
FC_ABI_STATIC_ASSERT(sizeof(FC_PatchDefinition) == 192, "x64 FC_PatchDefinition layout changed");
FC_ABI_STATIC_ASSERT(sizeof(FC_RegistrySink) == 24, "x64 FC_RegistrySink layout changed");
FC_ABI_STATIC_ASSERT(sizeof(FC_PluginApi) == 24, "x64 FC_PluginApi layout changed");
FC_ABI_STATIC_ASSERT(FC_ABI_ALIGNOF(FC_PluginApi) == 8, "x64 FC_PluginApi alignment changed");
FC_ABI_STATIC_ASSERT(offsetof(FC_PluginApi, register_plugin) == 16, "x64 FC_PluginApi callback offset changed");
FC_ABI_STATIC_ASSERT(sizeof(FC_CreateContext) == 48, "x64 FC_CreateContext layout changed");
FC_ABI_STATIC_ASSERT(FC_ABI_ALIGNOF(FC_CreateContext) == 8, "x64 FC_CreateContext alignment changed");
FC_ABI_STATIC_ASSERT(offsetof(FC_CreateContext, target) == 16, "x64 FC_CreateContext target offset changed");
FC_ABI_STATIC_ASSERT(sizeof(FC_PlanContext) == 48, "x64 FC_PlanContext layout changed");
FC_ABI_STATIC_ASSERT(FC_ABI_ALIGNOF(FC_PlanContext) == 8, "x64 FC_PlanContext alignment changed");
FC_ABI_STATIC_ASSERT(offsetof(FC_PlanContext, target) == 16, "x64 FC_PlanContext target offset changed");
FC_ABI_STATIC_ASSERT(sizeof(FC_PrepareContext) == 48, "x64 FC_PrepareContext layout changed");
FC_ABI_STATIC_ASSERT(FC_ABI_ALIGNOF(FC_PrepareContext) == 8, "x64 FC_PrepareContext alignment changed");
FC_ABI_STATIC_ASSERT(offsetof(FC_PrepareContext, resolve_data) == 24, "x64 FC_PrepareContext callback offset changed");
FC_ABI_STATIC_ASSERT(sizeof(FC_ActivateContext) == 16, "x64 FC_ActivateContext layout changed");
FC_ABI_STATIC_ASSERT(sizeof(FC_UpdateContext) == 16, "x64 FC_UpdateContext layout changed");
FC_ABI_STATIC_ASSERT(sizeof(FC_CpuContext) == 408, "x64 FC_CpuContext layout changed");
FC_ABI_STATIC_ASSERT(FC_ABI_ALIGNOF(FC_CpuContext) == 8, "x64 FC_CpuContext alignment changed");
FC_ABI_STATIC_ASSERT(offsetof(FC_CpuContext, rflags) == 256, "x64 FC_CpuContext flags offset changed");
FC_ABI_STATIC_ASSERT(offsetof(FC_CpuContext, resume_rsp) == 392, "x64 FC_CpuContext resume offset changed");
FC_ABI_STATIC_ASSERT(sizeof(FC_HookOwnerEntry) == 16, "x64 FC_HookOwnerEntry layout changed");
FC_ABI_STATIC_ASSERT(sizeof(FC_HookObserverEntry) == 32, "x64 FC_HookObserverEntry layout changed");
FC_ABI_STATIC_ASSERT(sizeof(FC_HookSnapshot) == 48, "x64 FC_HookSnapshot layout changed");
FC_ABI_STATIC_ASSERT(offsetof(FC_HookSnapshot, original) == 8, "x64 FC_HookSnapshot original offset changed");
FC_ABI_STATIC_ASSERT(offsetof(FC_HookSnapshot, observers) == 32, "x64 FC_HookSnapshot observers offset changed");
FC_ABI_STATIC_ASSERT(sizeof(FC_PluginDefinition) == 120, "x64 FC_PluginDefinition layout changed");
FC_ABI_STATIC_ASSERT(FC_ABI_ALIGNOF(FC_PluginDefinition) == 8, "x64 FC_PluginDefinition alignment changed");
FC_ABI_STATIC_ASSERT(offsetof(FC_PluginDefinition, id) == 8, "x64 FC_PluginDefinition ID offset changed");
FC_ABI_STATIC_ASSERT(offsetof(FC_PluginDefinition, patches) == 104, "x64 FC_PluginDefinition patches offset changed");
FC_ABI_STATIC_ASSERT(offsetof(FC_GroupDefinition, members) == 16, "x64 FC_GroupDefinition members offset changed");
FC_ABI_STATIC_ASSERT(offsetof(FC_SupportDefinition, callbacks) == 16,
                     "x64 FC_SupportDefinition callbacks offset changed");
FC_ABI_STATIC_ASSERT(offsetof(FC_PatchDefinition, supports) == 176, "x64 FC_PatchDefinition supports offset changed");
FC_ABI_STATIC_ASSERT(offsetof(FC_RegistrySink, submit) == 16, "x64 FC_RegistrySink callback offset changed");
FC_ABI_STATIC_ASSERT(sizeof(FC_HookBuildInput) == 32, "x64 FC_HookBuildInput layout changed");
FC_ABI_STATIC_ASSERT(FC_ABI_ALIGNOF(FC_HookBuildInput) == 8, "x64 FC_HookBuildInput alignment changed");
FC_ABI_STATIC_ASSERT(sizeof(FC_HookBuilder) == 16, "x64 FC_HookBuilder layout changed");
FC_ABI_STATIC_ASSERT(FC_ABI_ALIGNOF(FC_HookBuilder) == 8, "x64 FC_HookBuilder alignment changed");
FC_ABI_STATIC_ASSERT(sizeof(FC_HookRequest) == 160, "x64 FC_HookRequest layout changed");
FC_ABI_STATIC_ASSERT(FC_ABI_ALIGNOF(FC_HookRequest) == 8, "x64 FC_HookRequest alignment changed");
FC_ABI_STATIC_ASSERT(sizeof(FC_ObserverRequest) == 160, "x64 FC_ObserverRequest layout changed");
FC_ABI_STATIC_ASSERT(FC_ABI_ALIGNOF(FC_ObserverRequest) == 8, "x64 FC_ObserverRequest alignment changed");
FC_ABI_STATIC_ASSERT(offsetof(FC_HookBuildInput, struct_size) == 0, "x64 hook build input prefix changed");
FC_ABI_STATIC_ASSERT(offsetof(FC_HookBuildInput, entry) == 8, "x64 hook build input entry offset changed");
FC_ABI_STATIC_ASSERT(offsetof(FC_HookBuildInput, entry_size) == 16, "x64 hook build input size offset changed");
FC_ABI_STATIC_ASSERT(offsetof(FC_HookBuildInput, snapshot_slot) == 24, "x64 hook build input offset changed");
FC_ABI_STATIC_ASSERT(offsetof(FC_HookBuilder, build) == 0, "x64 hook builder callback offset changed");
FC_ABI_STATIC_ASSERT(offsetof(FC_HookBuilder, entry_size) == 8, "x64 hook builder size offset changed");
FC_ABI_STATIC_ASSERT(offsetof(FC_HookRequest, struct_size) == 0, "x64 hook request prefix changed");
FC_ABI_STATIC_ASSERT(offsetof(FC_HookRequest, location) == 8, "x64 hook request location offset changed");
FC_ABI_STATIC_ASSERT(offsetof(FC_HookRequest, kind) == 96, "x64 hook request kind offset changed");
FC_ABI_STATIC_ASSERT(offsetof(FC_HookRequest, native_call) == 104, "x64 hook request call offset changed");
FC_ABI_STATIC_ASSERT(offsetof(FC_HookRequest, builder) == 112, "x64 hook request builder offset changed");
FC_ABI_STATIC_ASSERT(offsetof(FC_HookRequest, context) == 128, "x64 hook request context offset changed");
FC_ABI_STATIC_ASSERT(offsetof(FC_HookRequest, callback) == 136, "x64 hook request callback offset changed");
FC_ABI_STATIC_ASSERT(offsetof(FC_HookRequest, original_context) == 144,
                     "x64 hook request original context offset changed");
FC_ABI_STATIC_ASSERT(offsetof(FC_HookRequest, bind_original) == 152, "x64 hook request tail offset changed");
FC_ABI_STATIC_ASSERT(offsetof(FC_ObserverRequest, struct_size) == 0, "x64 observer request prefix changed");
FC_ABI_STATIC_ASSERT(offsetof(FC_ObserverRequest, location) == 8, "x64 observer request location offset changed");
FC_ABI_STATIC_ASSERT(offsetof(FC_ObserverRequest, kind) == 96, "x64 observer request kind offset changed");
FC_ABI_STATIC_ASSERT(offsetof(FC_ObserverRequest, native_call) == 104, "x64 observer request call offset changed");
FC_ABI_STATIC_ASSERT(offsetof(FC_ObserverRequest, builder) == 112, "x64 observer request builder offset changed");
FC_ABI_STATIC_ASSERT(offsetof(FC_ObserverRequest, context) == 128, "x64 observer request context offset changed");
FC_ABI_STATIC_ASSERT(offsetof(FC_ObserverRequest, before) == 136, "x64 observer request before offset changed");
FC_ABI_STATIC_ASSERT(offsetof(FC_ObserverRequest, after) == 144, "x64 observer request after offset changed");
FC_ABI_STATIC_ASSERT(offsetof(FC_ObserverRequest, state_size) == 152, "x64 observer request state size offset changed");
FC_ABI_STATIC_ASSERT(offsetof(FC_ObserverRequest, state_alignment) == 156, "x64 observer request tail offset changed");
#endif

#undef FC_ABI_ALIGNOF
#undef FC_ABI_STATIC_ASSERT
