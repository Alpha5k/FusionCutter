#include "../../../src/core/runtime/interface_router.hpp"

#include <catch2/catch_test_macros.hpp>

#include <Windows.h>

#include <cstdint>
#include <string_view>

namespace {

// Matching source declarations intentionally prove layout agreement without sharing a private fixture header.
struct CounterV1 {
    static constexpr std::string_view id = "CounterV1";
    std::int32_t value{};
};

using QueryFn = FC_Bool(FC_CALL*)(void*, FC_PatchHandle, FC_StringView, std::uint32_t, void*) noexcept;
using DestroyFn = void(FC_CALL*)(void*, FC_PatchHandle) noexcept;

// This owner keeps the external code mapped until PatchInstance has released its callback handle.
class LoadedProvider final {
  public:
    LoadedProvider() {
        module_ = LoadLibraryW(FC_INTERFACE_PROVIDER_PATH);
    }
    LoadedProvider(const LoadedProvider&) = delete;
    LoadedProvider& operator=(const LoadedProvider&) = delete;
    ~LoadedProvider() {
        if (module_ != nullptr) {
            FreeLibrary(module_);
        }
    }

    [[nodiscard]] HMODULE get() const noexcept {
        return module_;
    }

  private:
    HMODULE module_{};
};

} // namespace

TEST_CASE("The production router queries an interface with exact layout through a separately built SDK DLL",
          "[runtime][interfaces][dll]") {
    // Resolve the native callback table from an independently compiled provider, as plugin admission would.
    LoadedProvider library;
    REQUIRE(library.get() != nullptr);
    const auto query = reinterpret_cast<QueryFn>(GetProcAddress(library.get(), "FC_Test_QueryInterface"));
    const auto destroy = reinterpret_cast<DestroyFn>(GetProcAddress(library.get(), "FC_Test_DestroyInterfaceProvider"));
    REQUIRE(query != nullptr);
    REQUIRE(destroy != nullptr);

    FC_PatchCallbacks callbacks{.destroy = destroy, .query_interface = query};
    fc::planning::PatchInstance instance{callbacks, reinterpret_cast<FC_PatchHandle>(1)};
    // Production publication and direct lookup must transport the copied contract without adaptation by the test.
    fc::runtime::InterfaceRouter router;
    router.reserve(1, 0);
    router.publish_provider(fc::catalog::PatchIndex{0}, instance, "Provider");
    router.mark_active(fc::catalog::PatchIndex{0});

    CounterV1 output{};
    REQUIRE(router.find_active("Provider", CounterV1::id, sizeof(output), &output) == FC_TRUE);
    CHECK(output.value == 91);
}
