#include <FusionCutter/LoaderApi.h>

#include <catch2/catch_test_macros.hpp>

#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

static_assert(sizeof(FC_HostRole) == sizeof(std::uint32_t));
static_assert(sizeof(FC_QueryResult) == sizeof(std::uint32_t));
static_assert(sizeof(FC_InitializeResult) == sizeof(std::uint32_t));
static_assert(sizeof(FC_LoaderKind) == sizeof(std::uint32_t));
static_assert(sizeof(FC_DirectInputChainOutcome) == sizeof(std::uint32_t));
static_assert(sizeof(FC_HostEventLevel) == sizeof(std::uint32_t));

static_assert(std::is_standard_layout_v<FC_LoaderStartupInfo>);
static_assert(std::is_standard_layout_v<FC_InitializeArgs>);
static_assert(std::is_standard_layout_v<FC_CoreApi>);

static_assert(sizeof(FC_LoaderStartupInfo) == 84);
static_assert(offsetof(FC_LoaderStartupInfo, struct_size) == 0);
static_assert(offsetof(FC_LoaderStartupInfo, loader_kind) == 4);
static_assert(offsetof(FC_LoaderStartupInfo, direct_input_chain_outcome) == 8);
static_assert(offsetof(FC_LoaderStartupInfo, windows_error) == 12);
static_assert(offsetof(FC_LoaderStartupInfo, wildcard_match_count) == 16);
static_assert(offsetof(FC_LoaderStartupInfo, selected_proxy_basename) == 20);

static_assert(sizeof(FC_InitializeArgs) == 92);
static_assert(offsetof(FC_InitializeArgs, struct_size) == 0);
static_assert(offsetof(FC_InitializeArgs, host_role) == 4);
static_assert(offsetof(FC_InitializeArgs, loader_startup) == 8);

static_assert(offsetof(FC_CoreApi, struct_size) == 0);
static_assert(offsetof(FC_CoreApi, abi_generation) == 4);
static_assert(offsetof(FC_CoreApi, supported_roles) == 8);

#if defined(_M_IX86)
static_assert(sizeof(FC_CoreApi) == 24);
static_assert(offsetof(FC_CoreApi, initialize) == 12);
static_assert(offsetof(FC_CoreApi, update) == 16);
static_assert(offsetof(FC_CoreApi, report_host_event) == 20);
#elif defined(_M_X64)
static_assert(sizeof(FC_CoreApi) == 40);
static_assert(offsetof(FC_CoreApi, initialize) == 16);
static_assert(offsetof(FC_CoreApi, update) == 24);
static_assert(offsetof(FC_CoreApi, report_host_event) == 32);
#else
#error Unsupported ABI test architecture
#endif

namespace {

using QueryCoreApiFn = decltype(&FusionCutter_GetCoreApi);

class LoadedModule {
  public:
    explicit LoadedModule(HMODULE handle) noexcept : handle_(handle) {}

    ~LoadedModule() {
        if (handle_ != nullptr) {
            FreeLibrary(handle_);
        }
    }

    LoadedModule(const LoadedModule&) = delete;
    LoadedModule& operator=(const LoadedModule&) = delete;

    [[nodiscard]] HMODULE get() const noexcept {
        return handle_;
    }

  private:
    HMODULE handle_;
};

struct GuardedCoreApi {
    FC_CoreApi table;
    std::array<std::uint8_t, 16> canary;
};

} // namespace

TEST_CASE("Generation-1 core ABI is stable and bounded", "[loaders][abi]") {
    std::array<wchar_t, 32'768> core_path{};
    const DWORD core_path_length =
        GetEnvironmentVariableW(L"FC_CORE_TEST_DLL", core_path.data(), static_cast<DWORD>(core_path.size()));
    const DWORD environment_error = core_path_length == 0 ? GetLastError() : ERROR_SUCCESS;
    INFO("GetEnvironmentVariableW error: " << environment_error);
    REQUIRE(core_path_length > 0);
    REQUIRE(core_path_length < core_path.size());

    const HMODULE module_handle = LoadLibraryW(core_path.data());
    const DWORD load_error = module_handle == nullptr ? GetLastError() : ERROR_SUCCESS;
    INFO("LoadLibraryW error: " << load_error);
    REQUIRE(module_handle != nullptr);
    const LoadedModule module(module_handle);

    const FARPROC export_address = GetProcAddress(module.get(), "FusionCutter_GetCoreApi");
    const DWORD export_error = export_address == nullptr ? GetLastError() : ERROR_SUCCESS;
    INFO("GetProcAddress error: " << export_error);
    REQUIRE(export_address != nullptr);

    const auto query_core_api = reinterpret_cast<QueryCoreApiFn>(export_address);

    FC_CoreApi api{};
    REQUIRE(query_core_api(FC_ABI_GENERATION, sizeof(api), &api) == FC_QUERY_OK);
    CHECK(api.struct_size == sizeof(FC_CoreApi));
    CHECK(api.abi_generation == FC_ABI_GENERATION);
    CHECK(api.supported_roles == FC_EXPECTED_ROLE_MASK);
    CHECK(api.initialize != nullptr);
    CHECK(api.update != nullptr);
    CHECK(api.report_host_event != nullptr);

    FC_InitializeArgs initialize_args{};
    initialize_args.struct_size = sizeof(initialize_args);
    initialize_args.host_role =
        (FC_EXPECTED_ROLE_MASK & FC_HOST_ROLE_CLIENT) != 0 ? FC_HOST_ROLE_CLIENT : FC_HOST_ROLE_SERVER;
    initialize_args.loader_startup.struct_size = sizeof(initialize_args.loader_startup);
    initialize_args.loader_startup.loader_kind = FC_LOADER_KIND_UNKNOWN;
    CHECK(api.initialize(&initialize_args) == FC_INIT_UNSUPPORTED);

    CHECK(query_core_api(FC_ABI_GENERATION, sizeof(api), nullptr) == FC_QUERY_INVALID_ARGUMENT);

    FC_CoreApi unsupported{};
    std::memset(&unsupported, 0xA5, sizeof(unsupported));
    const FC_CoreApi unsupported_before = unsupported;
    CHECK(query_core_api(FC_ABI_GENERATION + 1, sizeof(unsupported), &unsupported) == FC_QUERY_UNSUPPORTED_GENERATION);
    CHECK(std::memcmp(&unsupported, &unsupported_before, sizeof(unsupported)) == 0);

    GuardedCoreApi guarded{};
    std::memset(&guarded, 0xA5, sizeof(guarded));
    constexpr std::size_t kCallerCapacity = offsetof(FC_CoreApi, initialize);

    CHECK(query_core_api(FC_ABI_GENERATION, kCallerCapacity, &guarded.table) == FC_QUERY_TABLE_TOO_SMALL);
    CHECK(guarded.table.struct_size == sizeof(FC_CoreApi));
    CHECK(guarded.table.abi_generation == FC_ABI_GENERATION);
    CHECK(guarded.table.supported_roles == FC_EXPECTED_ROLE_MASK);

    const auto* guarded_bytes = reinterpret_cast<const std::uint8_t*>(&guarded);
    for (std::size_t index = kCallerCapacity; index < sizeof(guarded); ++index) {
        CAPTURE(index);
        REQUIRE(guarded_bytes[index] == 0xA5);
    }
}
