#pragma once

#include <fc/abi/common.h>

#ifdef __cplusplus
extern "C" {
#endif

// Describes the outcome of observing one named lobby item.
enum FC_LobbyResult {
    FC_LOBBY_VALUE = 0,   // Valid item in the requested format.
    FC_LOBBY_MISSING,     // Successful native read found no item.
    FC_LOBBY_MALFORMED,   // Stored FC record could not be decoded.
    FC_LOBBY_UNSUPPORTED, // Envelope or requested payload format is unsupported.
    FC_LOBBY_UNAVAILABLE, // Required native access is unavailable.
    FC_LOBBY_FAILED       // Native request or read failed.
};

// Contains one lobby read result; copy the record and payload if they are needed after the read callback returns.
typedef struct FC_LobbyRecord {
    // Native lobby ID; zero means absent.
    uint64_t lobby;
    uint32_t result;
    // Observed payload format when readable; failed requests supply no valid version.
    uint32_t version;
    FC_Bytes value;
    // Steady-clock nanoseconds at the successful native read, using the lifecycle clock's epoch; zero on failure.
    int64_t observed_ns;
} FC_LobbyRecord;

// Identifies a publication registration; the runtime keeps its address stable and owns its destruction.
typedef struct FC_LobbyItem FC_LobbyItem;

// Identifies a read request; the runtime keeps it alive, including after completion, until the caller calls cancel().
typedef struct FC_LobbyRequest FC_LobbyRequest;

// Receives a record and payload valid until return; the callback must not throw across the C boundary.
typedef void(FC_CDECL *FC_LobbyRead)(void *context, const FC_LobbyRecord *record);

// Provides lobby reads and publication; the table remains valid for the process lifetime.
// Call all operations on the runtime's control thread.
struct FC_LobbyApi {
    // Number of readable bytes in this table; check before reading an appended entry.
    uint32_t byte_size;
    // Copy name during prepare(); return 1 with an item or 0 with an error written before return.
    uint32_t(FC_CDECL *register_item)(void *owner, FC_Text name, uint32_t version, FC_LobbyItem **out, FC_ErrorSink);
    // Return the hosted or joined lobby identity, or zero when unavailable or absent.
    uint64_t(FC_CDECL *current)(void *owner);
    // For a cached record, call read once before returning 1; return 0 without calling read when no record is cached.
    // The runtime keeps neither context nor read after returning; reading does not reserve the name for publication.
    uint32_t(FC_CDECL *cached)(void *owner, uint64_t lobby, FC_Text name, uint32_t version, void *context, FC_LobbyRead read);
    // Copy lobby, name and version; return 1 with a request, or 0 with an error and no later callback.
    // Call read once on the control thread after refresh() returns; the caller must keep context alive until cancel().
    uint32_t(FC_CDECL *refresh)(void *owner, uint64_t lobby, FC_Text name, uint32_t version, void *context, FC_LobbyRead read,
                                FC_LobbyRequest **out, FC_ErrorSink);
    // Copy the latest whole value for publication in an owned lobby; empty bytes are valid.
    // Return 1 on acceptance or 0 with an error, preserving the previous accepted value on refusal.
    uint32_t(FC_CDECL *publish)(FC_LobbyItem *item, FC_Bytes value, FC_ErrorSink);
    // Withdraw this item and stop future republication until another publish.
    void(FC_CDECL *clear)(FC_LobbyItem *item);
    // Prevent further delivery and release all use of context before returning; native work may continue.
    // Call exactly once for each accepted request, even after completion; this releases its runtime record.
    void(FC_CDECL *cancel)(FC_LobbyRequest *request);
};

#ifdef __cplusplus
}
#endif
