#include "runtime_contract.h"

#include <FusionCutter/Abi.h>

#include <Windows.h>

#include <stdint.h>

#pragma section(".fcdata", read, write)

// Exported state lets the host distinguish physical original calls from plugin owner and observer counters.
__declspec(dllexport) __declspec(allocate(".fcdata")) FC_SliceLateState fc_slice_late_state = {0};

#pragma code_seg(push, late_site, ".fclate")

// The late target begins executing only after the framework has discovered and installed its participants.
__declspec(noinline) __declspec(dllexport) int32_t FC_CALL fc_slice_late_site(int32_t value) {
    InterlockedIncrement(&fc_slice_late_state.original_calls);
    return value;
}

#pragma code_seg(pop, late_site)
