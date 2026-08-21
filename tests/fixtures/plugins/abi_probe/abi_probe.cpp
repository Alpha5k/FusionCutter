#include <FusionCutter/PluginApi.h>

#include <Windows.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <string_view>

FC_EXTERN_C const FC_PluginApi* FC_CALL FusionCutter_QueryPlugin(std::uint32_t abi_generation) noexcept;

namespace {

// The staged filename selects one ABI behavior while all scenarios execute the same real plugin DLL.
enum class Mode {
    Valid,
    Alpha,
    Zulu,
    NoSubmission,
    MultipleSubmission,
    RegistrationFailure,
    InvalidStructure,
    SmallTable,
    HostTooLarge,
    NullRegister,
    LargeTable,
    OldSdk,
    ConfigurationFailure,
};

[[nodiscard]] Mode mode() noexcept {
    // Stage the same real plugin DLL under each scenario's filename so the admission matrix shares one fixture.
    HMODULE module{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&FusionCutter_QueryPlugin), &module)) {
        return Mode::Valid;
    }
    std::array<wchar_t, 512> path{};
    const auto size = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    if (size == 0 || size >= path.size()) {
        return Mode::Valid;
    }
    const std::wstring_view name{path.data(), size};
    if (name.find(L"Alpha") != std::wstring_view::npos) {
        return Mode::Alpha;
    }
    if (name.find(L"Zulu") != std::wstring_view::npos) {
        return Mode::Zulu;
    }
    if (name.find(L"NoSubmission") != std::wstring_view::npos) {
        return Mode::NoSubmission;
    }
    if (name.find(L"MultipleSubmission") != std::wstring_view::npos) {
        return Mode::MultipleSubmission;
    }
    if (name.find(L"RegistrationFailure") != std::wstring_view::npos) {
        return Mode::RegistrationFailure;
    }
    if (name.find(L"InvalidStructure") != std::wstring_view::npos) {
        return Mode::InvalidStructure;
    }
    if (name.find(L"SmallTable") != std::wstring_view::npos) {
        return Mode::SmallTable;
    }
    if (name.find(L"HostTooLarge") != std::wstring_view::npos) {
        return Mode::HostTooLarge;
    }
    if (name.find(L"NullRegister") != std::wstring_view::npos) {
        return Mode::NullRegister;
    }
    if (name.find(L"LargeTable") != std::wstring_view::npos) {
        return Mode::LargeTable;
    }
    if (name.find(L"OldSdk") != std::wstring_view::npos) {
        return Mode::OldSdk;
    }
    if (name.find(L"ConfigurationFailure") != std::wstring_view::npos) {
        return Mode::ConfigurationFailure;
    }
    return Mode::Valid;
}

[[nodiscard]] constexpr FC_StringView text(std::string_view value) noexcept {
    return {value.data(), static_cast<std::uint32_t>(value.size())};
}

// Minimal inert lifecycle callbacks keep every admitted probe focused on its query or plugin registration boundary.
FC_CallStatus FC_CALL create_patch(void*, const FC_CreateContext*, const FC_SettingsView*, const FC_ErrorSink*,
                                   FC_PatchHandle* output) {
    *output = reinterpret_cast<FC_PatchHandle>(std::uintptr_t{1});
    return FC_CALL_OK;
}

void FC_CALL destroy_patch(void*, FC_PatchHandle) {}

FC_CallStatus FC_CALL plan_patch(void*, FC_PatchHandle, const FC_PlanContext*, const FC_PlanSink*,
                                 const FC_ErrorSink*) {
    return FC_CALL_OK;
}

// Exercises registration status, submission count, structural content, and configuration participation independently.
FC_CallStatus FC_CALL register_plugin(const FC_HostApi*, const FC_RegistrySink* registry, const FC_ErrorSink* error) {
    const auto selected_mode = mode();
    if (selected_mode == Mode::NoSubmission) {
        return FC_CALL_OK;
    }
    if (selected_mode == Mode::RegistrationFailure) {
        if (error != nullptr && error->set != nullptr) {
            error->set(error->context, text("The admission probe requested registration failure"),
                       text("Register probe"));
        }
        return FC_CALL_FAILED;
    }

    // All scenarios share one valid tree, changing only the field needed by the selected admission boundary.
    const auto plugin_id = selected_mode == Mode::Alpha                  ? std::string_view{"AlphaProbe"}
                           : selected_mode == Mode::Zulu                 ? std::string_view{"ZuluProbe"}
                           : selected_mode == Mode::ConfigurationFailure ? std::string_view{"ConfigurationFailureProbe"}
                                                                         : std::string_view{"AdmissionProbe"};
    const auto patch_id = selected_mode == Mode::Alpha              ? std::string_view{"AlphaPatch"}
                          : selected_mode == Mode::Zulu             ? std::string_view{"ZuluPatch"}
                          : selected_mode == Mode::InvalidStructure ? std::string_view{"General"}
                                                                    : std::string_view{"AdmissionPatch"};
    const FC_PatchCallbacks callbacks{.create = &create_patch, .destroy = &destroy_patch, .plan = &plan_patch};
    const FC_SupportDefinition support{
        .layouts = selected_mode == Mode::ConfigurationFailure ? FC_LAYOUT_STEAM_RETAIL : FC_LAYOUT_GAMESPY_RETAIL,
        .roles = FC_HOST_ROLE_CLIENT,
        .image = FC_IMAGE_GAME,
        .callbacks = callbacks,
        .failure_policy = FC_FAILURE_INHERIT};
    const FC_PatchDefinition patch{.id = text(patch_id),
                                   .name = text("Admission probe patch"),
                                   .configurable = FC_TRUE,
                                   .enabled = FC_FALSE,
                                   .failure_policy = FC_FAILURE_CONTINUE,
                                   .supports = &support,
                                   .support_count = 1};
    const FC_PluginDefinition plugin{
        .struct_size = sizeof(FC_PluginDefinition), .id = text(plugin_id), .patches = &patch, .patch_count = 1};

    const auto first = registry->submit(registry->context, &plugin);
    if (selected_mode == Mode::MultipleSubmission) {
        (void)registry->submit(registry->context, &plugin);
    }
    return first == FC_SUBMIT_ACCEPTED ? FC_CALL_OK : FC_CALL_FAILED;
}

// Appends an unknown tail to prove admission copies only the supported FC_PluginApi prefix.
struct ExtendedPluginApi {
    FC_PluginApi api;
    std::uint64_t tail;
};

} // namespace

#if defined(_M_IX86)
#pragma comment(linker, "/export:FusionCutter_QueryPlugin=_FusionCutter_QueryPlugin")
#else
#pragma comment(linker, "/export:FusionCutter_QueryPlugin")
#endif

FC_EXTERN_C const FC_PluginApi* FC_CALL FusionCutter_QueryPlugin(std::uint32_t abi_generation) noexcept {
    if (abi_generation != FC_PLUGIN_ABI_GENERATION) {
        return nullptr;
    }
    // Static table variants probe prefix sizing and forward compatibility without transient pointer lifetimes.
    static const FC_PluginApi normal{.struct_size = sizeof(FC_PluginApi),
                                     .sdk_revision = FC_SDK_REVISION,
                                     .host_api_size = sizeof(FC_HostApi),
                                     .register_plugin = &register_plugin};
    static const FC_PluginApi small{.struct_size = offsetof(FC_PluginApi, register_plugin),
                                    .sdk_revision = FC_SDK_REVISION,
                                    .host_api_size = sizeof(FC_HostApi),
                                    .register_plugin = &register_plugin};
    static const FC_PluginApi large_host{.struct_size = sizeof(FC_PluginApi),
                                         .sdk_revision = FC_SDK_REVISION,
                                         .host_api_size = sizeof(FC_HostApi) + 1,
                                         .register_plugin = &register_plugin};
    static const FC_PluginApi null_registration{
        .struct_size = sizeof(FC_PluginApi), .sdk_revision = FC_SDK_REVISION, .host_api_size = sizeof(FC_HostApi)};
    static const ExtendedPluginApi extended{{.struct_size = sizeof(ExtendedPluginApi),
                                             .sdk_revision = FC_SDK_REVISION,
                                             .host_api_size = sizeof(FC_HostApi),
                                             .register_plugin = &register_plugin},
                                            UINT64_C(0x1122334455667788)};
    static const FC_PluginApi old_sdk{.struct_size = sizeof(FC_PluginApi),
                                      .sdk_revision = 0,
                                      .host_api_size = sizeof(FC_HostApi),
                                      .register_plugin = &register_plugin};

    switch (mode()) {
    case Mode::SmallTable:
        return &small;
    case Mode::HostTooLarge:
        return &large_host;
    case Mode::NullRegister:
        return &null_registration;
    case Mode::LargeTable:
        return &extended.api;
    case Mode::OldSdk:
        return &old_sdk;
    default:
        return &normal;
    }
}
