#pragma once

#include <fc/abi/common.h>

#ifdef __cplusplus
extern "C" {
#endif

// Describes how this process participates in a session, including LAN; this is separate from its launch mode.
enum FC_GameMode {
    FC_GAME_LOCAL = 0,     // Singleplayer mission.
    FC_GAME_HOST = 1,      // Hosting multiplayer with a local player.
    FC_GAME_CLIENT = 2,    // Joining or playing on another host.
    FC_GAME_DEDICATED = 3, // Hosting multiplayer without a local player.
    FC_GAME_NONE = 4       // No active session.
};

// Contains Game state copied from the runtime.
typedef struct FC_GameState {
    // Process-local monotonic ID for a local mission run or multiplayer hosting/joining attempt; zero means no session.
    uint64_t session;
    // One FC_GameMode value describing current participation.
    uint32_t mode;
} FC_GameState;

// Calls the patch on the serialized control thread with state valid until return; null means unavailable state.
// The callback must not throw across the C API and must copy any state it retains.
typedef void(FC_CDECL *FC_GameChange)(void *context, const FC_GameState *state);

// Provides Game state and callback registration for each patch; the table lives until process exit.
struct FC_GameApi {
    // Check this prefix before accessing an appended entry.
    uint32_t byte_size;

    // Copies current state without blocking on any thread; out must be writable.
    // Returns 1 for known state, including None with session zero, or 0 for unavailable state.
    uint32_t(FC_CDECL *current)(void *owner, FC_GameState *out);

    // Registers during Prepare; returns 1 on success or 0 with a synchronous error. Keep callback_context through handler destruction.
    // Calls once after patch activation and then on state changes; rapid changes may combine into the latest state.
    uint32_t(FC_CDECL *on_change)(void *owner, void *callback_context, FC_GameChange, FC_ErrorSink);
};

#ifdef __cplusplus
}
#endif
