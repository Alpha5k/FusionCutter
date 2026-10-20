#include <fc/abi.h>
#include <Windows.h>
#include <atomic>
#include <cstring>

namespace {

std::atomic_uint32_t start_count{};
FC_StartOptions captured{};
std::uint32_t captured_abi{};
} // namespace

// Captures valid startup requests; FC_TEST_CORE_RESULT=failed reports an error and returns zero.
// On x64, success also requires the selected original game DLL to be loaded.
extern "C" std::uint32_t FC_CDECL FC_Start(std::uint32_t abi, const FC_StartOptions *options, FC_ErrorSink error) noexcept {
    if (abi != FC_ABI_VERSION || !options || options->byte_size < sizeof(FC_StartOptions) || options->dedicated > 1) {
        return 0;
    }

    // Delay startup so concurrent loader calls have time to overlap it.
    Sleep(25);

    // Publish the captured arguments before the count lets readers observe the request.
    captured = *options;
    captured_abi = abi;
    start_count.fetch_add(1, std::memory_order_release);

    char mode[32]{};
    GetEnvironmentVariableA("FC_TEST_CORE_RESULT", mode, sizeof(mode));
    if (std::strcmp(mode, "failed") == 0) {
        constexpr char message[] = "Fixture initialization rejected the request";
        if (error.write) {
            error.write(error.context, {message, sizeof(message) - 1}, ERROR_ACCESS_DENIED);
        }
        return 0;
    }
#if defined(_WIN64)
    // The original module must already be loaded when the Classic proxy enters FC_Start.
    if (!GetModuleHandleW(L"Battlefront1.original.dll") && !GetModuleHandleW(L"Battlefront2.original.dll")) {
        return 0;
    }
#endif
    return 1;
}

// Returns the number of captured startup requests, including requests that later failed.
extern "C" std::uint32_t FC_Test_StartCount() noexcept {
    return start_count.load(std::memory_order_acquire);
}

// Call after FC_Start finishes; copies the captured options and ABI.
// Returns FALSE for null outputs or before any request was captured.
extern "C" BOOL FC_Test_CopyStartup(FC_StartOptions *options, std::uint32_t *abi) noexcept {
    if (!options || !abi || !start_count.load(std::memory_order_acquire)) {
        return FALSE;
    }

    *options = captured;
    *abi = captured_abi;
    return TRUE;
}
