#include <FusionCutter/LoaderApi.h>

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace {

std::atomic_uint32_t g_initialize_count{};
std::atomic_uint32_t g_update_count{};
std::atomic_uint32_t g_host_event_count{};
std::atomic_bool g_required_module_loaded{true};
FC_InitializeArgs g_arguments{};
FC_HostEventLevel g_last_event_level{};
std::array<char, 256> g_last_event{};

[[nodiscard]] bool environment_is(const char* name, std::string_view expected) noexcept {
    std::array<char, 64> value{};
    const auto length = GetEnvironmentVariableA(name, value.data(), static_cast<DWORD>(value.size()));
    return length == expected.size() && std::string_view(value.data(), length) == expected;
}

FC_InitializeResult FC_CALL initialize(const FC_InitializeArgs* arguments) noexcept {
    g_initialize_count.fetch_add(1, std::memory_order_relaxed);
    if (arguments != nullptr && arguments->struct_size >= sizeof(FC_InitializeArgs)) {
        g_arguments = *arguments;
    }

    std::array<wchar_t, 260> required_module{};
    const auto required_length = GetEnvironmentVariableW(L"FC_STUB_REQUIRED_MODULE", required_module.data(),
                                                         static_cast<DWORD>(required_module.size()));
    if (required_length != 0) {
        const bool loaded =
            required_length < required_module.size() && GetModuleHandleW(required_module.data()) != nullptr;
        g_required_module_loaded.store(loaded, std::memory_order_relaxed);
    }
    if (environment_is("FC_STUB_INIT_RESULT", "Fatal")) {
        return FC_INIT_FATAL;
    }
    if (environment_is("FC_STUB_INIT_RESULT", "Unsupported")) {
        return FC_INIT_UNSUPPORTED;
    }
    return FC_INIT_COMPLETED;
}

void FC_CALL update() noexcept {
    g_update_count.fetch_add(1, std::memory_order_relaxed);
}

void FC_CALL report_host_event(FC_HostEventLevel level, const char* message) noexcept {
    g_host_event_count.fetch_add(1, std::memory_order_relaxed);
    g_last_event_level = level;
    g_last_event.fill('\0');
    if (message != nullptr) {
        const auto length = std::min(std::strlen(message), g_last_event.size() - 1);
        std::memcpy(g_last_event.data(), message, length);
    }
}

constexpr FC_CoreApi kApi{
    sizeof(FC_CoreApi), FC_ABI_GENERATION, FC_HOST_ROLE_CLIENT | FC_HOST_ROLE_SERVER, initialize, update,
    report_host_event,
};

} // namespace

extern "C" FC_QueryResult FC_CALL FusionCutter_GetCoreApi(std::uint32_t requested_generation,
                                                          std::uint32_t caller_table_capacity,
                                                          FC_CoreApi* output_table) {
    if (output_table == nullptr) {
        return FC_QUERY_INVALID_ARGUMENT;
    }
    if (environment_is("FC_STUB_QUERY_MODE", "Unsupported") || requested_generation != FC_ABI_GENERATION) {
        return FC_QUERY_UNSUPPORTED_GENERATION;
    }

    const auto copy_size = std::min<std::size_t>(caller_table_capacity, sizeof(kApi));
    std::memcpy(output_table, &kApi, copy_size);
    if (environment_is("FC_STUB_QUERY_MODE", "InvalidTable")) {
        output_table->struct_size = 0;
    }
    return caller_table_capacity < sizeof(kApi) ? FC_QUERY_TABLE_TOO_SMALL : FC_QUERY_OK;
}

extern "C" std::uint32_t FC_CALL FC_Test_GetInitializeCount() noexcept {
    return g_initialize_count.load(std::memory_order_relaxed);
}

extern "C" std::uint32_t FC_CALL FC_Test_GetUpdateCount() noexcept {
    return g_update_count.load(std::memory_order_relaxed);
}

extern "C" BOOL FC_CALL FC_Test_CopyInitializeArgs(FC_InitializeArgs* output) noexcept {
    if (output == nullptr) {
        return FALSE;
    }
    *output = g_arguments;
    return TRUE;
}

extern "C" std::uint32_t FC_CALL FC_Test_GetHostEventCount() noexcept {
    return g_host_event_count.load(std::memory_order_relaxed);
}

extern "C" BOOL FC_CALL FC_Test_WasRequiredModuleLoaded() noexcept {
    return g_required_module_loaded.load(std::memory_order_relaxed) ? TRUE : FALSE;
}

extern "C" FC_HostEventLevel FC_CALL FC_Test_GetLastHostEventLevel() noexcept {
    return g_last_event_level;
}

extern "C" const char* FC_CALL FC_Test_GetLastHostEvent() noexcept {
    return g_last_event.data();
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void*) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}
