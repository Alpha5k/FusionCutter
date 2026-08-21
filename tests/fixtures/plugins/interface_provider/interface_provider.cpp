#include <FusionCutter/SDK.hpp>

#include <cstdint>
#include <string_view>

namespace {

// This plain contract crosses a real test DLL boundary through the public SDK adapter that verifies its exact layout.
struct CounterV1 {
    static constexpr std::string_view id = "CounterV1";
    std::int32_t value{};
};

// The real SDK adapter constructs InterfaceQuery's private request state inside this separately built DLL.
struct Provider {
    void query_interface(fc::InterfaceQuery& query) noexcept {
        query.provide(CounterV1{91});
    }
};

Provider provider;

} // namespace

// The exported callback is shaped exactly like an admitted patch's query slot; only its construction stays test-only.
extern "C" __declspec(dllexport) FC_Bool FC_CALL FC_Test_QueryInterface(void*, FC_PatchHandle, FC_StringView id,
                                                                        std::uint32_t size, void* output) noexcept {
    return fc::detail::InterfaceQueryAdapter::query(provider, id, size, output) ? FC_TRUE : FC_FALSE;
}

// No instance-owned resource exists; this no-op completes the real PatchInstance callback table used by the test.
extern "C" __declspec(dllexport) void FC_CALL FC_Test_DestroyInterfaceProvider(void*, FC_PatchHandle) noexcept {}
