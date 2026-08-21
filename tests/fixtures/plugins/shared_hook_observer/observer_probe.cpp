#include "../shared_hook_probe.hpp"

#include <FusionCutter/SDK.hpp>

#include <winsock2.h>

#include <cstdint>

#if defined(_M_IX86)
#pragma comment(linker, "/export:FcHookProbeAfterThunk=_FcHookProbeAfterThunk")
#pragma comment(linker, "/export:FcHookProbeBeforeThunk=_FcHookProbeBeforeThunk")
#pragma comment(linker, "/export:FcHookProbeGetObserverBuilder=_FcHookProbeGetObserverBuilder")
#endif

namespace {

// A per-invocation checksum detects state aliasing when dispatch is nested or concurrent.
struct ObservationState {
    std::int32_t expected_result{};
};

} // namespace

// These exports keep builder generation and observer callbacks inside a DLL distinct from the physical entry owner.
extern "C" __declspec(dllexport) void FC_CALL FcHookProbeGetObserverBuilder(FC_HookBuilder* output) noexcept {
    if (output != nullptr) {
        *output = fc::detail::typed_hook_builder<FcHookProbeCall>();
    }
}

extern "C" __declspec(dllexport) void FC_CALL FcHookProbeBeforeThunk(void* context, std::int32_t left,
                                                                     std::int32_t right, void* state) noexcept {
    auto& observer = *static_cast<FcHookProbeObserverContext*>(context);
    InterlockedIncrement(&observer.before_calls);
    auto& observation = *static_cast<ObservationState*>(state);
    // Value initialization and per-call storage both require this field to be fresh before the checksum is recorded.
    if (observation.expected_result != 0) {
        InterlockedIncrement(&observer.state_mismatches);
    }
    observation.expected_result = left + right;
    // Deliberately corrupt both ambient APIs; the dispatcher must hide these writes from every later participant.
    SetLastError(0x1111);
    WSASetLastError(0x2222);
}

extern "C" __declspec(dllexport) void FC_CALL FcHookProbeAfterThunk(void* context, std::int32_t left,
                                                                    std::int32_t right, std::int32_t result,
                                                                    const void* state) noexcept {
    auto& observer = *static_cast<FcHookProbeObserverContext*>(context);
    InterlockedIncrement(&observer.after_calls);
    const auto& observation = *static_cast<const ObservationState*>(state);
    if (observation.expected_result != left + right || result < observation.expected_result) {
        InterlockedIncrement(&observer.state_mismatches);
    }
    SetLastError(0x3333);
    WSASetLastError(0x4444);
}
