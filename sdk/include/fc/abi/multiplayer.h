#pragma once

#include <fc/abi/common.h>

#ifdef __cplusplus
extern "C" {
#endif

// Chooses whether the patch implements client behavior, host behavior, or both.
enum FC_FeatureRole {
    FC_FEATURE_CLIENT = 1,
    FC_FEATURE_HOST = 2, // Includes dedicated servers.
    FC_FEATURE_BOTH = 3
};

// Describes feature compatibility on the current connection.
enum FC_Agreement {
    FC_AGREEMENT_UNAVAILABLE = 0, // No applicable connection, or the feature is inactive.
    FC_AGREEMENT_PENDING,         // The runtime is checking peer support.
    FC_AGREEMENT_COMPATIBLE,      // Client and host support the same feature ID.
    FC_AGREEMENT_INCOMPATIBLE,    // The peer lacks matching support, or the deadline expired.
    FC_AGREEMENT_FAILED           // This feature or its shared connection failed.
};

// Accepted means the runtime copied the complete message; every refusal submits no message.
enum FC_SendResult {
    FC_SEND_ACCEPTED = 0,
    FC_SEND_UNAVAILABLE,  // No active connection, feature, or applicable send direction.
    FC_SEND_PENDING,      // The runtime is checking peer support.
    FC_SEND_INCOMPATIBLE, // The peer lacks matching feature support.
    FC_SEND_FULL,         // The queue cannot accept the message immediately, including lock contention.
    FC_SEND_TOO_LARGE,    // The message exceeds max_payload().
    FC_SEND_FAILED        // This feature or its shared connection failed.
};

// Contains the native connection slot and Galaxy user ID associated with a peer.
typedef struct FC_PeerInfo {
    // Nonzero means slot is present; zero is a valid native slot.
    uint32_t has_slot;
    // Native connection slot, which need not be a player index.
    uint32_t slot;
    // Nonzero means galaxy_id is present.
    uint32_t has_galaxy_id;
    // Galaxy user ID; a remote host's lobby is resolved to its owner.
    uint64_t galaxy_id;
} FC_PeerInfo;

// Describes a feature to register; the runtime copies the exact ID during prepare().
typedef struct FC_FeatureDefinition {
    FC_Text id;
    uint32_t role;
} FC_FeatureDefinition;

// Identifies a feature registration; the runtime keeps its address stable and owns its destruction.
typedef struct FC_Feature FC_Feature;

// Receives portions of the client list during clients(); the runtime keeps neither pointer after the call.
typedef struct FC_PeerListSink {
    void *context;
    // Return 0 to stop copying, including after a caught allocation failure; do not throw across this boundary.
    uint32_t(FC_CDECL *write)(void *context, const uint64_t *peers, uint32_t count);
} FC_PeerListSink;

// Receives agreement changes on the runtime's control thread; calls are serialized and must not throw.
typedef void(FC_CDECL *FC_PeerChange)(void *context, uint64_t peer, uint32_t agreement);

// Receives one complete message on the runtime's control thread; calls are serialized and must not throw.
// Copy any bytes needed after the callback returns.
typedef void(FC_CDECL *FC_FeatureMessage)(void *context, uint64_t peer, FC_Bytes);

// Provides peer queries and feature registration; the table remains valid for the process lifetime.
struct FC_MultiplayerApi {
    uint32_t byte_size;
    // Return current endpoint IDs, or zero when absent; a playing host has distinct host and local-client IDs.
    uint64_t(FC_CDECL *host)(void *owner);
    uint64_t(FC_CDECL *local)(void *owner);
    // Copy the host's clients on the control thread, including its local client; stop if the sink returns 0.
    void(FC_CDECL *clients)(void *owner, FC_PeerListSink);
    // Copy current native associations; return 0 for an unavailable or expired peer, otherwise 1.
    uint32_t(FC_CDECL *info)(void *owner, uint64_t peer, FC_PeerInfo *out);
    // Copy the definition during prepare(); return 1 and a registration, or 0 with an error written before return.
    uint32_t(FC_CDECL *register_feature)(void *owner, const FC_FeatureDefinition *, FC_Feature **out, FC_ErrorSink);
    // Return one FC_Agreement value for this registration and the current connection.
    uint32_t(FC_CDECL *agreement)(FC_Feature *, uint64_t peer);
    // Copy accepted bytes without waiting; return one FC_SendResult value. Local-client messages also enter the queue.
    uint32_t(FC_CDECL *send)(FC_Feature *, uint64_t peer, FC_Bytes);
    // Return the maximum message size in bytes, allowing space for the frame header and feature ID.
    uint32_t(FC_CDECL *max_payload)(FC_Feature *);
    // Fail this feature's communication with the peer, copying the reason during the call.
    void(FC_CDECL *fail)(FC_Feature *, uint64_t peer, FC_Text reason);
    // Register during prepare(); return 1 on success or 0 with an error written before return.
    // The patch's handler must keep capture alive until the handler is destroyed.
    uint32_t(FC_CDECL *on_peer)(FC_Feature *, void *capture, FC_PeerChange, FC_ErrorSink);
    // Register during prepare(); return 1 on success or 0 with an error written before return.
    // The patch's handler must keep capture alive until the handler is destroyed.
    uint32_t(FC_CDECL *on_message)(FC_Feature *, void *capture, FC_FeatureMessage, FC_ErrorSink);
};

#ifdef __cplusplus
}
#endif
