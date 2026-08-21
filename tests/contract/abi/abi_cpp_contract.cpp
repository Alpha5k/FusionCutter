// Mirror the C hostile-packing probe so both language modes protect the includer's packing state.
#pragma pack(push, 1)

#include <FusionCutter/CoreApi.h>
#include <FusionCutter/PluginApi.h>

struct OuterPackProbe {
    uint8_t prefix;
    uint64_t value;
};

static_assert(offsetof(OuterPackProbe, value) == 1);

#pragma pack(pop)

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <type_traits>

extern "C" int fc_abi_c_contract(void);

namespace {

// Function types and record traits verify that the declarations retain a C-compatible surface for ABI generation 1.
using QueryCore = const FC_CoreApi*(FC_CALL*)(std::uint32_t) noexcept;
using QueryPlugin = const FC_PluginApi*(FC_CALL*)(std::uint32_t) noexcept;

static_assert(std::is_same_v<decltype(&FusionCutter_QueryCore), QueryCore>);
static_assert(std::is_same_v<decltype(&FusionCutter_QueryPlugin), QueryPlugin>);
static_assert(std::is_standard_layout_v<FC_CoreApi>);
static_assert(std::is_standard_layout_v<FC_PluginApi>);
static_assert(std::is_standard_layout_v<FC_PluginDefinition>);
static_assert(std::is_trivially_copyable_v<FC_PatchCallbacks>);
static_assert(offsetof(FC_HookBuilder, entry_size) == sizeof(void*));

TEST_CASE("ABI generation 1 is identical in C and C++") {
    CHECK(fc_abi_c_contract() == 0);
    CHECK(FusionCutter_QueryCore(FC_CORE_ABI_GENERATION) == nullptr);
    CHECK(FusionCutter_QueryPlugin(FC_PLUGIN_ABI_GENERATION) == nullptr);
}

} // namespace
