#pragma once

#include <stdint.h>

// These macros set the calling convention and export attribute. The DLL must still expose the
// undecorated FC_GetPlugin name on x86 and x64.
#if defined(_MSC_VER)
#define FC_CDECL __cdecl
#define FC_EXPORT __declspec(dllexport)
#else
#define FC_CDECL
#define FC_EXPORT
#endif

// Plugins and the runtime must agree on this value before the runtime reads an FC_Plugin record.
#define FC_ABI_VERSION 2u

#ifdef __cplusplus
extern "C" {
#endif

// Non-owning UTF-8 text view passed across the C ABI; null termination is optional.
typedef struct FC_Text {
    // May be null only when size is zero.
    const char *data;
    // Byte count excluding any null terminator.
    uint32_t size;
} FC_Text;

// Receives synchronous errors reported across the C ABI; the callback must not throw across the boundary.
typedef struct FC_ErrorSink {
    // Passed unchanged whenever write is called.
    void *context;
    // May be null. When called, runs synchronously; copy message before returning if it must be retained.
    // windows_error is zero when no Windows error code applies.
    void(FC_CDECL *write)(void *context, FC_Text message, uint32_t windows_error);
} FC_ErrorSink;

// Non-owning byte view; the enclosing call or declaration determines its storage lifetime.
typedef struct FC_Bytes {
    // May be null only when size is zero.
    const uint8_t *data;
    // Byte count.
    uint32_t size;
} FC_Bytes;

typedef struct FC_NativeApi FC_NativeApi;
typedef struct FC_PatchApi FC_PatchApi;
typedef struct FC_GameApi FC_GameApi;
typedef struct FC_MultiplayerApi FC_MultiplayerApi;
typedef struct FC_LobbyApi FC_LobbyApi;
typedef struct FC_ReportingApi FC_ReportingApi;

// Supplies runtime API tables to a handler; the host and available tables remain valid while the handler uses them.
typedef struct FC_Host {
    // Check this prefix size before reading a field added by a later ABI.
    uint32_t byte_size;
    // A null table means that facility is unavailable.
    const FC_NativeApi *native;
    const FC_PatchApi *patch_api;
    const FC_GameApi *game;
    const FC_MultiplayerApi *multiplayer;
    const FC_LobbyApi *lobby;
    const FC_ReportingApi *reporting;
} FC_Host;

#ifdef __cplusplus
}
#endif
