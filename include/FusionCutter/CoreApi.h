#pragma once

#include <FusionCutter/Abi.h>

#include <stddef.h>

#define FC_CORE_ABI_GENERATION UINT32_C(1)

// Initialization and loader diagnostics use fixed values so loaders can report failure without framework-owned text.
typedef uint32_t FC_InitializeResult;
#define FC_INIT_COMPLETED UINT32_C(0)
#define FC_INIT_UNSUPPORTED UINT32_C(1)
#define FC_INIT_FATAL UINT32_C(2)

// Identifies the loader path that entered the framework so startup diagnostics retain their original context.
typedef uint32_t FC_LoaderKind;
#define FC_LOADER_KIND_UNKNOWN UINT32_C(0)
#define FC_LOADER_KIND_DINPUT8 UINT32_C(1)
#define FC_LOADER_KIND_RCONSERVER UINT32_C(2)
#define FC_LOADER_KIND_BATTLEFRONT2 UINT32_C(3)

// Records how the DirectInput proxy chain was resolved before framework initialization began.
typedef uint32_t FC_DirectInputChainResult;
#define FC_DIRECT_INPUT_CHAIN_NOT_APPLICABLE UINT32_C(0)
#define FC_DIRECT_INPUT_CHAIN_SYSTEM_ONLY UINT32_C(1)
#define FC_DIRECT_INPUT_CHAIN_LOADED UINT32_C(2)
#define FC_DIRECT_INPUT_CHAIN_INVALID UINT32_C(3)
#define FC_DIRECT_INPUT_CHAIN_AMBIGUOUS UINT32_C(4)

#define FC_SELECTED_PROXY_BASENAME_CAPACITY UINT32_C(64)

#pragma pack(push, 8)

// Fixed by-value startup diagnostics supplied by the loader; this child record is not independently extensible.
typedef struct FC_LoaderStartupInfo {
    FC_LoaderKind loader_kind;
    FC_DirectInputChainResult direct_input_chain;
    uint32_t windows_error;
    uint32_t wildcard_match_count;
    char selected_proxy_basename[FC_SELECTED_PROXY_BASENAME_CAPACITY];
} FC_LoaderStartupInfo;

// Extensible top-level input copied by the framework during the serialized initialization call.
typedef struct FC_InitializeArgs {
    uint32_t struct_size;
    FC_HostRole host_role;
    FC_LoaderStartupInfo loader_startup;
} FC_InitializeArgs;

// Initialization runs serially outside loader lock; the loader pump invokes the bounded Update callback.
typedef FC_InitializeResult(FC_CALL* FC_InitializeFn)(const FC_InitializeArgs* args);
typedef void(FC_CALL* FC_UpdateFn)(void);

// Immutable process table returned by FusionCutter_QueryCore for the lifetime of the loaded FusionCutter.dll.
typedef struct FC_CoreApi {
    uint32_t struct_size;
    FC_InitializeFn initialize;
    FC_UpdateFn update;
} FC_CoreApi;

#pragma pack(pop)

// Returns the immutable table for a supported generation, or null without side effects for an unsupported generation.
FC_EXTERN_C const FC_CoreApi* FC_CALL FusionCutter_QueryCore(uint32_t abi_generation) FC_NOEXCEPT;

#if defined(__cplusplus)
#define FC_CORE_STATIC_ASSERT(expression, message) static_assert((expression), message)
#define FC_CORE_ALIGNOF(type) alignof(type)
#else
#define FC_CORE_STATIC_ASSERT(expression, message) _Static_assert((expression), message)
#define FC_CORE_ALIGNOF(type) _Alignof(type)
#endif

FC_CORE_STATIC_ASSERT(sizeof(FC_LoaderStartupInfo) == 80, "FC_LoaderStartupInfo layout changed");
FC_CORE_STATIC_ASSERT(FC_CORE_ALIGNOF(FC_LoaderStartupInfo) == 4, "FC_LoaderStartupInfo alignment changed");
FC_CORE_STATIC_ASSERT(offsetof(FC_LoaderStartupInfo, selected_proxy_basename) == 16,
                      "FC_LoaderStartupInfo field offset changed");
FC_CORE_STATIC_ASSERT(sizeof(FC_InitializeArgs) == 88, "FC_InitializeArgs layout changed");
FC_CORE_STATIC_ASSERT(FC_CORE_ALIGNOF(FC_InitializeArgs) == 4, "FC_InitializeArgs alignment changed");
FC_CORE_STATIC_ASSERT(offsetof(FC_InitializeArgs, loader_startup) == 8, "FC_InitializeArgs field offset changed");

#if defined(_M_IX86)
FC_CORE_STATIC_ASSERT(sizeof(FC_CoreApi) == 12, "x86 FC_CoreApi layout changed");
FC_CORE_STATIC_ASSERT(FC_CORE_ALIGNOF(FC_CoreApi) == 4, "x86 FC_CoreApi alignment changed");
FC_CORE_STATIC_ASSERT(offsetof(FC_CoreApi, initialize) == 4, "x86 FC_CoreApi initialize offset changed");
FC_CORE_STATIC_ASSERT(offsetof(FC_CoreApi, update) == 8, "x86 FC_CoreApi update offset changed");
#elif defined(_M_X64)
FC_CORE_STATIC_ASSERT(sizeof(FC_CoreApi) == 24, "x64 FC_CoreApi layout changed");
FC_CORE_STATIC_ASSERT(FC_CORE_ALIGNOF(FC_CoreApi) == 8, "x64 FC_CoreApi alignment changed");
FC_CORE_STATIC_ASSERT(offsetof(FC_CoreApi, initialize) == 8, "x64 FC_CoreApi initialize offset changed");
FC_CORE_STATIC_ASSERT(offsetof(FC_CoreApi, update) == 16, "x64 FC_CoreApi update offset changed");
#endif

#undef FC_CORE_ALIGNOF
#undef FC_CORE_STATIC_ASSERT
