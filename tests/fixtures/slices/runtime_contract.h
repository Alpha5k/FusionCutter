#pragma once

#include <Windows.h>

#include <stddef.h>
#include <stdint.h>

// Shared state makes native effects observable without adding callbacks used only by tests to the CoreApi ABI.
typedef struct FC_SliceProviderState {
    volatile LONG owner_calls;
    volatile LONG instruction_calls;
    volatile LONG service_calls;
    volatile LONG update_calls;
    LONG activate_original_result;
} FC_SliceProviderState;

// The observer record distinguishes paired callbacks, interface discovery, and per-invocation state failures.
typedef struct FC_SliceObserverState {
    volatile LONG before_calls;
    volatile LONG after_calls;
    volatile LONG state_mismatches;
    volatile LONG service_calls;
    volatile LONG immediate_result;
    volatile LONG bound_result;
    volatile LONG update_calls;
} FC_SliceObserverState;

// The host owns this data while independently built plugins receive only reviewed RVAs into the image.
typedef struct FC_SliceHostState {
    volatile uint32_t simple_value;
    FC_SliceProviderState provider;
    FC_SliceObserverState observer;
    volatile LONG typed_original_calls;
    volatile LONG instruction_original_calls;
    volatile LONG worker_failures;
    unsigned char layout_padding[0x2000];
} FC_SliceHostState;

// The late image records physical calls while each plugin retains its own lifecycle counters for live status.
typedef struct FC_SliceLateState {
    volatile LONG original_calls;
} FC_SliceLateState;

#if defined(_M_IX86)
// Architecture-specific totals preserve the same logical mixed call while its physical homes differ.
#define FC_SLICE_HOST_DATA_RVA 0x6000u
#define FC_SLICE_TYPED_ORIGINAL_RESULT 16
#define FC_SLICE_TYPED_HOOKED_RESULT 21
#define FC_SLICE_ACTIVATE_ORIGINAL_RESULT 13
#else
#define FC_SLICE_HOST_DATA_RVA 0x7000u
#define FC_SLICE_TYPED_ORIGINAL_RESULT 25
#define FC_SLICE_TYPED_HOOKED_RESULT 30
#define FC_SLICE_ACTIVATE_ORIGINAL_RESULT 15
#endif

// Provider and observer records are stable children of the host's reviewed writable data section.
#define FC_SLICE_PROVIDER_STATE_RVA (FC_SLICE_HOST_DATA_RVA + (uint32_t)offsetof(FC_SliceHostState, provider))
#define FC_SLICE_OBSERVER_STATE_RVA (FC_SLICE_HOST_DATA_RVA + (uint32_t)offsetof(FC_SliceHostState, observer))

// Dedicated linker sections keep these reviewed locations stable across both supported architectures.
#define FC_SLICE_TYPED_SITE_RVA 0x2000u
#define FC_SLICE_INSTRUCTION_SITE_RVA 0x3000u
#define FC_SLICE_LATE_SITE_RVA 0x1000u
