#pragma once

#include <fc/abi/common.h>

#ifdef __cplusplus
extern "C" {
#endif

enum FC_LogLevel {
    FC_LOG_OFF,
    FC_LOG_ERROR,
    FC_LOG_WARNING,
    FC_LOG_INFO,
    FC_LOG_DEBUG
};

// Receives formatted text synchronously; the context and callback remain valid during the formatter call.
typedef struct FC_TextSink {
    void *context;
    // Copies text before return; 1 means accepted, and 0 tells the formatter that output failed.
    uint32_t(FC_CDECL *write)(void *, FC_Text);
} FC_TextSink;

// Borrows packet bytes and the output sink for this call; formats on the reporting worker and returns 1 on success.
typedef uint32_t(FC_CDECL *FC_LogFormatFn)(FC_Bytes copied, FC_TextSink output);

// Supplies bytes to copy into the log queue and a formatter to interpret them later.
typedef struct FC_LogPacket {
    FC_Bytes copied;
    // The callback's module must remain loaded until process exit.
    FC_LogFormatFn format;
} FC_LogPacket;

// Requests a named channel for opaque trace records owned by one patch.
typedef struct FC_TraceDefinition {
    // Unique within the patch; copied during creation.
    FC_Text name;
    // Author-defined payload format version included in emitted records.
    uint32_t format_version;
    // Queue budget in bytes, including runtime record headers.
    uint64_t buffer_bytes;
} FC_TraceDefinition;

// Captures one channel's delivery counters and the shared trace output state.
typedef struct FC_TraceHealth {
    // Records copied into the queue, accepted by ETW, and lost during queuing or output.
    uint64_t accepted, written, dropped;
    uint32_t output_failed, file_limit_reached;
} FC_TraceHealth;

// Connects plugin logging, trace channels and fatal errors to the runtime's reporting services.
struct FC_ReportingApi {
    // Check the available prefix before accessing an appended callback.
    uint32_t byte_size;
    void *context;
    // Returns 1 when the configured severity filter admits this level.
    uint32_t(FC_CDECL *log_enabled)(void *, void *owner, uint32_t level);
    // Copies packet bytes before return; 1 means accepted into the queue for worker formatting.
    // Owner records and the formatter's module remain valid until process exit.
    uint32_t(FC_CDECL *log_submit)(void *, void *owner, uint32_t level, const FC_LogPacket *);
    // Copies text before return; filtered or malformed records return 0 without loss, while valid refusals count as losses.
    uint32_t(FC_CDECL *log_write)(void *, void *owner, uint32_t level, FC_Text);
    // Creates only for a Preparing owner and copies the definition; the channel address remains valid until exit.
    uint32_t(FC_CDECL *trace)(void *, void *owner, const FC_TraceDefinition *, void **out, FC_ErrorSink);
    // Returns 1 when the channel is enabled and output is available; a null channel returns 0.
    uint32_t(FC_CDECL *trace_enabled)(void *, void *channel);
    // Returns 1 after copying into the queue; valid capacity/output refusals count as dropped.
    // Disabled, malformed and oversized records return 0 without increasing the loss count.
    uint32_t(FC_CDECL *trace_write)(void *, void *channel, FC_Bytes);
    // Copies current counters and flags; a null channel produces zero values.
    void(FC_CDECL *trace_health)(void *, void *channel, FC_TraceHealth *out);
    // Copies diagnostic facts and terminates; reporting failure cannot return control to the patch.
    void(FC_CDECL *fatal)(void *, void *owner, FC_Text, uint32_t windows_error);
};

#ifdef __cplusplus
}
#endif
