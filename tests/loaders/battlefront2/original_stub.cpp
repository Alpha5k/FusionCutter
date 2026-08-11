#include "original_stub.hpp"

#include <Windows.h>

#include <atomic>
#include <cstdint>

namespace {

std::atomic_uint32_t g_call_count{};
fusioncutter::tests::GameWinMainArguments g_arguments{};

} // namespace

extern "C" std::uint32_t GameWinMain(std::uint64_t argument1, std::uint64_t argument2, std::uint64_t argument3,
                                     std::uint64_t argument4, std::uint8_t* argument5, std::uint32_t argument6,
                                     std::uint64_t argument7) noexcept {
    g_arguments = {argument1, argument2, argument3, argument4, argument5, argument6, argument7};
    g_call_count.fetch_add(1, std::memory_order_relaxed);
    return fusioncutter::tests::kGameWinMainResult;
}

extern "C" std::uint32_t FC_Test_GetGameWinMainCallCount() noexcept {
    return g_call_count.load(std::memory_order_relaxed);
}

extern "C" BOOL FC_Test_CopyGameWinMainArguments(fusioncutter::tests::GameWinMainArguments* output) noexcept {
    if (output == nullptr) {
        return FALSE;
    }
    *output = g_arguments;
    return TRUE;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void*) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}
