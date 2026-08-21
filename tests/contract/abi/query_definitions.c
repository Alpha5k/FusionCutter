#include <FusionCutter/CoreApi.h>
#include <FusionCutter/PluginApi.h>

// Linkable C stubs prove both query declarations can be defined with their published calling conventions.
FC_EXTERN_C const FC_CoreApi* FC_CALL FusionCutter_QueryCore(uint32_t abi_generation) FC_NOEXCEPT {
    (void)abi_generation;
    return 0;
}

FC_EXTERN_C const FC_PluginApi* FC_CALL FusionCutter_QueryPlugin(uint32_t abi_generation) FC_NOEXCEPT {
    (void)abi_generation;
    return 0;
}
