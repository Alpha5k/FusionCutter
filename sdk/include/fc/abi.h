#pragma once

#include <fc/abi/common.h>
#include <fc/abi/game.h>
#include <fc/abi/lobby.h>
#include <fc/abi/multiplayer.h>
#include <fc/abi/native.h>
#include <fc/abi/reporting.h>
#include <fc/abi/plugin.h>

#ifdef __cplusplus
extern "C" {
#endif

// Describes a patch API's table layout and signatures; Prepare publication or connection copies its nested views.
typedef struct FC_ApiContract {
    // Exact contract identity used to distinguish APIs offered by a provider.
    FC_Text id;
    // Complete table size and required alignment, both in bytes.
    uint32_t table_size, table_alignment;
    // Opaque layout/signature bytes must match between provider and consumer.
    FC_Bytes description;
    // Byte offsets of every required function pointer, excluding its context field.
    const uint32_t *required_functions;
    uint32_t function_count;
} FC_ApiContract;

// Receives the retained provider table once both provider and consumer are installed.
typedef void(FC_CDECL *FC_ApiAvailable)(void *context, const void *table);
// Runs a consumer operation while the runtime checks its installed state.
typedef void(FC_CDECL *FC_ConsumerInvoke)(void *context);

// Reserves patch APIs during Prepare and checks installed consumer activity; retained tables last until process exit.
struct FC_PatchApi {
    // Check the supplied prefix before accessing an appended callback.
    uint32_t byte_size;
    // Copies contract and table bytes for publication after successful installation; returns 1 on success.
    uint32_t(FC_CDECL *publish)(void *owner, const FC_ApiContract *, const void *table, FC_ErrorSink);
    // Copies an installed declared provider's matching table into caller storage; returns 1 on success.
    uint32_t(FC_CDECL *get)(void *owner, FC_Text provider, const FC_ApiContract *, void *out_table, FC_ErrorSink);
    // Registers one delivery after both patches activate; context belongs to the consumer's retained capture storage.
    uint32_t(FC_CDECL *on_available)(void *owner, FC_Text provider, const FC_ApiContract *, void *context, FC_ApiAvailable, FC_ErrorSink);
    // Returns 0 before activation or after failed startup; live faults terminate after reporting.
    uint32_t(FC_CDECL *invoke)(void *owner, FC_ConsumerInvoke, void *context);
};

// Supplies the record size and launch mode for a runtime startup request.
typedef struct FC_StartOptions {
    // Size of this record in bytes; must cover the dedicated field.
    uint32_t byte_size;
    // Dedicated server uses 1; normal client launch uses 0.
    uint32_t dedicated;
} FC_StartOptions;

// Called outside loader lock with matching ABI and options; waits for control-thread initialization.
// Returns 1 after successful initialization or 0 with an optional synchronous error.
typedef uint32_t(FC_CDECL *FC_StartFn)(uint32_t abi, const FC_StartOptions *, FC_ErrorSink error);

#ifdef __cplusplus
}
#endif
