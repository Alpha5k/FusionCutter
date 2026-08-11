#pragma once

#include <stdint.h>

#if defined(_MSC_VER)
#define FC_CALL __cdecl
#else
#define FC_CALL
#endif

#define FC_ABI_GENERATION UINT32_C(1)

typedef uint32_t FC_HostRole;

#define FC_HOST_ROLE_CLIENT UINT32_C(1)
#define FC_HOST_ROLE_SERVER UINT32_C(2)

typedef uint32_t FC_QueryResult;

#define FC_QUERY_OK UINT32_C(0)
#define FC_QUERY_INVALID_ARGUMENT UINT32_C(1)
#define FC_QUERY_UNSUPPORTED_GENERATION UINT32_C(2)
#define FC_QUERY_TABLE_TOO_SMALL UINT32_C(3)

typedef uint32_t FC_InitializeResult;

#define FC_INIT_COMPLETED UINT32_C(0)
#define FC_INIT_UNSUPPORTED UINT32_C(1)
#define FC_INIT_FATAL UINT32_C(2)

typedef uint32_t FC_LoaderKind;

#define FC_LOADER_KIND_UNKNOWN UINT32_C(0)
#define FC_LOADER_KIND_DINPUT8 UINT32_C(1)
#define FC_LOADER_KIND_RCONSERVER UINT32_C(2)
#define FC_LOADER_KIND_BATTLEFRONT2 UINT32_C(3)

typedef uint32_t FC_DirectInputChainOutcome;

#define FC_DIRECT_INPUT_CHAIN_NOT_APPLICABLE UINT32_C(0)
#define FC_DIRECT_INPUT_CHAIN_SYSTEM_ONLY UINT32_C(1)
#define FC_DIRECT_INPUT_CHAIN_LOADED UINT32_C(2)
#define FC_DIRECT_INPUT_CHAIN_INVALID UINT32_C(3)
#define FC_DIRECT_INPUT_CHAIN_AMBIGUOUS UINT32_C(4)

typedef uint32_t FC_HostEventLevel;

#define FC_HOST_EVENT_ERROR UINT32_C(1)
#define FC_HOST_EVENT_WARNING UINT32_C(2)
#define FC_HOST_EVENT_INFO UINT32_C(3)
#define FC_HOST_EVENT_DEBUG UINT32_C(4)

#define FC_SELECTED_PROXY_BASENAME_CAPACITY UINT32_C(64)

typedef struct FC_LoaderStartupInfo {
    uint32_t struct_size;
    FC_LoaderKind loader_kind;
    FC_DirectInputChainOutcome direct_input_chain_outcome;
    uint32_t windows_error;
    uint32_t wildcard_match_count;
    char selected_proxy_basename[FC_SELECTED_PROXY_BASENAME_CAPACITY];
} FC_LoaderStartupInfo;

typedef struct FC_InitializeArgs {
    uint32_t struct_size;
    FC_HostRole host_role;
    FC_LoaderStartupInfo loader_startup;
} FC_InitializeArgs;

typedef FC_InitializeResult(FC_CALL* FC_InitializeFn)(const FC_InitializeArgs* args);
typedef void(FC_CALL* FC_UpdateFn)(void);
typedef void(FC_CALL* FC_ReportHostEventFn)(FC_HostEventLevel level, const char* message);

typedef struct FC_CoreApi {
    uint32_t struct_size;
    uint32_t abi_generation;
    uint32_t supported_roles;
    FC_InitializeFn initialize;
    FC_UpdateFn update;
    FC_ReportHostEventFn report_host_event;
} FC_CoreApi;

#ifdef __cplusplus
extern "C" {
#endif

FC_QueryResult FC_CALL FusionCutter_GetCoreApi(uint32_t requested_generation, uint32_t caller_table_capacity,
                                               FC_CoreApi* output_table);

#ifdef __cplusplus
}
#endif
