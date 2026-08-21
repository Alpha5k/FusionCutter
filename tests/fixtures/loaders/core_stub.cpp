#include <FusionCutter/CoreApi.h>

#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>

namespace {

// The child process reads these counters and the copied request to observe loader behavior across the DLL boundary.
std::atomic_uint32_t initialize_count{};
std::atomic_uint32_t update_count{};
FC_InitializeArgs captured{};

// A process-local environment value lets one ABI table drive every possible initialization result.
[[nodiscard]] FC_InitializeResult configured_result() noexcept {
    char value[32]{};
    const auto size = GetEnvironmentVariableA("FC_TEST_CORE_RESULT", value, static_cast<DWORD>(sizeof(value)));
    if (size != 0 && std::strcmp(value, "unsupported") == 0) {
        return FC_INIT_UNSUPPORTED;
    }
    if (size != 0 && std::strcmp(value, "fatal") == 0) {
        return FC_INIT_FATAL;
    }
    return FC_INIT_COMPLETED;
}

FC_InitializeResult FC_CALL initialize(const FC_InitializeArgs* arguments) noexcept {
    if (arguments == nullptr || arguments->struct_size < sizeof(FC_InitializeArgs)) {
        return FC_INIT_FATAL;
    }
    // The delay overlaps production entry calls at the common std::call_once boundary.
    Sleep(25);
    captured = *arguments;
    initialize_count.fetch_add(1, std::memory_order_release);
    return configured_result();
}

// Pump activity is reduced to a counter so the child distinguishes Completed and Unsupported initialization results.
void FC_CALL update() noexcept {
    update_count.fetch_add(1, std::memory_order_relaxed);
}

// The table is intentionally indistinguishable from a negotiated FusionCutter.dll at the loader boundary.
const FC_CoreApi api{.struct_size = sizeof(FC_CoreApi), .initialize = &initialize, .update = &update};

} // namespace

// These inspection exports expose only evidence produced by the negotiated callbacks above.
FC_EXTERN_C const FC_CoreApi* FC_CALL FusionCutter_QueryCore(uint32_t generation) FC_NOEXCEPT {
    return generation == FC_CORE_ABI_GENERATION ? &api : nullptr;
}

extern "C" std::uint32_t FC_Test_InitializeCount() noexcept {
    return initialize_count.load(std::memory_order_acquire);
}

extern "C" std::uint32_t FC_Test_UpdateCount() noexcept {
    return update_count.load(std::memory_order_relaxed);
}

extern "C" BOOL FC_Test_CopyStartup(FC_InitializeArgs* output) noexcept {
    if (output == nullptr || initialize_count.load(std::memory_order_acquire) == 0) {
        return FALSE;
    }
    *output = captured;
    return TRUE;
}
