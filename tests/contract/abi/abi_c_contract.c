// Including both public headers under hostile packing proves each header restores the caller's ABI state.
#pragma pack(push, 1)

#include <FusionCutter/CoreApi.h>
#include <FusionCutter/PluginApi.h>

typedef struct FC_OuterPackProbe {
    uint8_t prefix;
    uint64_t value;
} FC_OuterPackProbe;

_Static_assert(offsetof(FC_OuterPackProbe, value) == 1, "public headers must restore the includer's packing");

#pragma pack(pop)

// C compilation independently verifies shared prefixes, masks, and architecture-specific table sizes.
_Static_assert(sizeof(FC_StringView) == sizeof(void*) * 2, "FC_StringView layout changed");
_Static_assert(sizeof(FC_ByteView) == sizeof(void*) * 2, "FC_ByteView layout changed");
_Static_assert(offsetof(FC_InitializeArgs, loader_startup) == 8, "FC_InitializeArgs prefix changed");
_Static_assert(offsetof(FC_PluginApi, register_plugin) % sizeof(void*) == 0, "plugin query callback is misaligned");
_Static_assert(offsetof(FC_PluginDefinition, id) % sizeof(void*) == 0, "plugin definition ID is misaligned");
_Static_assert(offsetof(FC_PrepareContext, resolve_data) % sizeof(void*) == 0, "Prepare callback is misaligned");
_Static_assert(FC_HOOK_ENTRY_MAX_SIZE == UINT32_C(4096), "hook entry capacity changed");
_Static_assert(FC_CORE_ABI_GENERATION == UINT32_C(1), "framework ABI generation changed");
_Static_assert(FC_PLUGIN_ABI_GENERATION == UINT32_C(1), "plugin ABI generation changed");
_Static_assert(FC_HOST_ROLE_ALL == (FC_HOST_ROLE_CLIENT | FC_HOST_ROLE_SERVER), "host role mask changed");
_Static_assert(FC_LAYOUT_ALL == (FC_LAYOUT_GAMESPY_RETAIL | FC_LAYOUT_STEAM_RETAIL | FC_LAYOUT_GOG_RETAIL |
                                 FC_LAYOUT_MOD_TOOLS | FC_LAYOUT_CLASSIC_COLLECTION),
               "target layout mask changed");

#if defined(_M_IX86)
_Static_assert(sizeof(FC_TargetInfo) == 20, "x86 FC_TargetInfo layout changed");
_Static_assert(sizeof(FC_SettingsView) == 12, "x86 FC_SettingsView layout changed");
_Static_assert(sizeof(FC_PatchCallbacks) == 36, "x86 FC_PatchCallbacks layout changed");
_Static_assert(sizeof(FC_SupportDefinition) == 80, "x86 FC_SupportDefinition layout changed");
_Static_assert(sizeof(FC_PatchDefinition) == 100, "x86 FC_PatchDefinition layout changed");
_Static_assert(sizeof(FC_PlanSink) == 40, "x86 FC_PlanSink layout changed");
_Static_assert(sizeof(FC_StatusSink) == 28, "x86 FC_StatusSink layout changed");
#elif defined(_M_X64)
_Static_assert(sizeof(FC_TargetInfo) == 32, "x64 FC_TargetInfo layout changed");
_Static_assert(sizeof(FC_SettingsView) == 24, "x64 FC_SettingsView layout changed");
_Static_assert(sizeof(FC_PatchCallbacks) == 72, "x64 FC_PatchCallbacks layout changed");
_Static_assert(sizeof(FC_SupportDefinition) == 144, "x64 FC_SupportDefinition layout changed");
_Static_assert(sizeof(FC_PatchDefinition) == 192, "x64 FC_PatchDefinition layout changed");
_Static_assert(sizeof(FC_PlanSink) == 80, "x64 FC_PlanSink layout changed");
_Static_assert(sizeof(FC_StatusSink) == 56, "x64 FC_StatusSink layout changed");
#endif

// Linking this symbol into the C++ test proves the C translation unit completed every static contract check.
int fc_abi_c_contract(void) {
    return 0;
}
