#include "runtime_contract.h"
#include "simple_write_constants.h"

#include <FusionCutter/CoreApi.h>

#include <winsock2.h>
#include <Windows.h>

#include <stdint.h>

#pragma section(".fcdata", read, write)

// One stable data record lets the simple and runtime slices inspect real mutations in the child process.
__declspec(allocate(".fcdata")) FC_SliceHostState fc_slice_state = {
    FC_SIMPLE_WRITE_INITIAL_VALUE,
};

// These call types match the public query boundary and the native fixture functions invoked after installation.
typedef const FC_CoreApi*(FC_CALL* QueryCore)(uint32_t);
#if defined(_M_IX86)
typedef int32_t(__fastcall* SliceCall)(int32_t, int32_t, int32_t);
#else
typedef int32_t(FC_CALL* SliceCall)(int32_t, int32_t, int32_t, int32_t, int32_t);
#endif
typedef int32_t(FC_CALL* LateCall)(int32_t);

// Skip DLL teardown because ExitProcess can strand locks held by terminated framework background threads.
__declspec(noreturn) static void finish(UINT code) {
    TerminateProcess(GetCurrentProcess(), code);
    __assume(0);
}

#pragma code_seg(push, typed_site, ".fctyped")

// This ordinary function entry is shared by one modifying owner and one transparent observer in other DLLs.
__declspec(noinline) __declspec(dllexport) int32_t
#if defined(_M_IX86)
    __fastcall
#else
    FC_CALL
#endif
    fc_slice_typed_site(int32_t first, int32_t second, int32_t third
#if defined(_M_X64)
                        ,
                        int32_t fourth, int32_t fifth
#endif
    ) {
    InterlockedIncrement(&fc_slice_state.typed_original_calls);
    SetLastError(0x7001);
    WSASetLastError(0x7002);
    return first + second + third
#if defined(_M_X64)
           + fourth + fifth
#endif
           + 10;
}

#pragma code_seg(pop, typed_site)
#pragma code_seg(push, instruction_site, ".fcinst")

// The instruction fixture executes displaced code after a plugin callback observes the live CPU context.
__declspec(noinline) __declspec(dllexport) void FC_CALL fc_slice_instruction_site(void) {
    InterlockedIncrement(&fc_slice_state.instruction_original_calls);
}

#pragma code_seg(pop, instruction_site)

// Calls the physical mixed ABI through the compiler convention represented by the plugin's explicit layout.
static int32_t invoke_typed(SliceCall call) {
#if defined(_M_IX86)
    return call(1, 2, 3);
#else
    return call(1, 2, 3, 4, 5);
#endif
}

// Concurrent callers prove generated hook dispatch and observer state remain isolated across native threads.
static DWORD WINAPI call_worker(void* context) {
    const SliceCall call = (SliceCall)context;
    unsigned int iteration;
    for (iteration = 0; iteration < 64; ++iteration) {
        SetLastError(0x6001);
        WSASetLastError(0x6002);
        if (invoke_typed(call) != FC_SLICE_TYPED_HOOKED_RESULT || GetLastError() != 0x7002 ||
            WSAGetLastError() != 0x7002) {
            InterlockedIncrement(&fc_slice_state.worker_failures);
        }
    }
    return 0;
}

// The private host reports a real loader tuple for each architecture so ordinary target recognition runs unchanged.
#if defined(_M_IX86)
static const FC_InitializeArgs initialization = {
    sizeof(FC_InitializeArgs),
    FC_HOST_ROLE_SERVER,
    {FC_LOADER_KIND_RCONSERVER, FC_DIRECT_INPUT_CHAIN_NOT_APPLICABLE, 0, 0, {0}},
};
#else
static const FC_InitializeArgs initialization = {
    sizeof(FC_InitializeArgs),
    FC_HOST_ROLE_SERVER,
    {FC_LOADER_KIND_RCONSERVER, FC_DIRECT_INPUT_CHAIN_NOT_APPLICABLE, 0, 0, {0}},
};
#endif

// A CRT-free entry keeps x86/x64 PE facts deterministic while crossing only the published CoreApi ABI.
void __cdecl fc_slice_entry(void) {
    HANDLE workers[4] = {0};
    const HMODULE core = LoadLibraryW(L"FusionCutter.dll");
    QueryCore query;
    const FC_CoreApi* api;
    int32_t result;
    unsigned int index;

    // Load and negotiate the framework solely through the published boundary used by every loader.
    if (core == NULL) {
        finish(1);
    }
    query = (QueryCore)GetProcAddress(core, "FusionCutter_QueryCore");
    if (query == NULL) {
        finish(2);
    }
    api = query(FC_CORE_ABI_GENERATION);
    if (api == NULL || api->struct_size < sizeof(FC_CoreApi) || api->initialize == NULL || api->update == NULL) {
        finish(3);
    }
    if (api->initialize(&initialization) != FC_INIT_COMPLETED) {
        finish(4);
    }

    // A process containing only the simple plugin exits before imposing expectations from the runtime fixture.
    result = invoke_typed(&fc_slice_typed_site);
    if (result == FC_SLICE_TYPED_ORIGINAL_RESULT) {
        if (fc_slice_state.simple_value != FC_SIMPLE_WRITE_DEFAULT_REPLACEMENT &&
            fc_slice_state.simple_value != FC_SIMPLE_WRITE_OVERRIDE_REPLACEMENT) {
            finish(5);
        }
        finish(0);
    }
    if (result != FC_SLICE_TYPED_HOOKED_RESULT) {
        finish(6);
    }
    if (GetLastError() != 0x7002) {
        finish(16);
    }
    if (WSAGetLastError() != 0x7002) {
        finish(17);
    }

    // Exercise the same installed dispatcher from four native threads while the main thread remains independent.
    for (index = 0; index < 4; ++index) {
        workers[index] = CreateThread(NULL, 0, &call_worker, (void*)&fc_slice_typed_site, 0, NULL);
        if (workers[index] == NULL) {
            finish(7);
        }
    }
    if (WaitForMultipleObjects(4, workers, TRUE, INFINITE) != WAIT_OBJECT_0) {
        finish(8);
    }
    for (index = 0; index < 4; ++index) {
        CloseHandle(workers[index]);
    }
    if (fc_slice_state.worker_failures != 0 || fc_slice_state.provider.owner_calls != 257 ||
        fc_slice_state.typed_original_calls != 258 ||
        fc_slice_state.provider.activate_original_result != FC_SLICE_ACTIVATE_ORIGINAL_RESULT) {
        finish(9);
    }

    // A deliberate observer Prepare callback failure is valid; otherwise every owner call retains its observation.
    if (!((fc_slice_state.observer.before_calls == 0 && fc_slice_state.observer.after_calls == 0) ||
          (fc_slice_state.observer.before_calls == 257 && fc_slice_state.observer.after_calls == 257))) {
        finish(10);
    }
    if (fc_slice_state.observer.state_mismatches != 0) {
        finish(11);
    }

    // The instruction boundary must run both the plugin callback and the displaced physical function body once.
    fc_slice_instruction_site();
    if (fc_slice_state.provider.instruction_calls != 1 || fc_slice_state.instruction_original_calls != 1) {
        finish(12);
    }

#if defined(_M_IX86)
    {
        // Loading the peer after startup forces the normal Update pump to recognize, validate, and install late work.
        HMODULE peer = LoadLibraryW(L"FusionCutterSlicePeer.dll");
        FC_SliceLateState* late_state;
        LateCall late_site;
        if (peer == NULL) {
            finish(13);
        }
        for (index = 0; index < 40; ++index) {
            api->update();
            Sleep(25);
        }
        late_state = (FC_SliceLateState*)GetProcAddress(peer, "fc_slice_late_state");
        late_site = (LateCall)GetProcAddress(peer, "fc_slice_late_site");
        if (late_state == NULL || late_site == NULL || late_site(20) != 27) {
            finish(14);
        }
        if (late_state->original_calls != 1) {
            finish(15);
        }
    }
#endif

    // The final pump publishes call counters, trace health, and any continued failure into live status.
    Sleep(1100);
    api->update();
    finish(0);
}
