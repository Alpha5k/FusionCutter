#include "simple_write.hpp"
#include "synthetic_image.hpp"

#include <FusionCutter/Testing.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

// Keeps Scenario's borrowed loaded-layout images alive while the bundled contribution is validated.
struct ScenarioInput {
    fc::test::Scenario scenario;
    std::vector<std::vector<std::byte>> images;
};

// This repeats only public input construction; the linked Testing owner differs solely by bundled acquisition.
[[nodiscard]] ScenarioInput bundled_scenario() {
    std::array<std::byte, sizeof(std::uint32_t)> payload{};
    std::memcpy(payload.data(), &fc::fixtures::simple_write::kInitialValue, payload.size());
#if defined(_M_IX86)
    const fc::TargetInfo target{fc::TargetLayout::SteamRetail, fc::HostRole::Client, fc::Architecture::X86,
                                "SteamRetail_Game_59EDE353"};
    ScenarioInput result{fc::test::Scenario{target.layout, target.role}};
    result.images.push_back(fc::fixtures::synthetic_image(target.architecture, 0x400000,
                                                          fc::fixtures::simple_write::value_rva(target), payload));
    result.scenario.add_image(fc::TargetImage::Game, std::string{target.image_profile}, result.images.back());
#else
    const fc::TargetInfo target{fc::TargetLayout::ClassicCollection, fc::HostRole::Client, fc::Architecture::X64,
                                "ClassicCollection_Game_66702CD2"};
    ScenarioInput result{fc::test::Scenario{target.layout, target.role}};
    result.images.push_back(
        fc::fixtures::synthetic_image(target.architecture, 0x2000, {0x1000}, std::span<const std::byte>{}));
    result.scenario.add_image(fc::TargetImage::Bootstrap, "ClassicCollection_Bootstrap_66702CD7", result.images.back());
    result.images.push_back(fc::fixtures::synthetic_image(target.architecture, 0x700000,
                                                          fc::fixtures::simple_write::value_rva(target), payload));
    result.scenario.add_image(fc::TargetImage::Game, std::string{target.image_profile}, result.images.back());
#endif
    return result;
}

} // namespace

TEST_CASE("simple checked-write bundled contribution matches the external public result") {
    auto input = bundled_scenario();
    const auto result = input.scenario.validate();
    REQUIRE(result);

    // The logical result matches external admission, while the absent DLL path proves bundled acquisition was used.
    const auto* plugin = result->find_plugin(fc::fixtures::simple_write::kPluginId);
    REQUIRE(plugin != nullptr);
    CHECK(plugin->admitted);
    CHECK_FALSE(plugin->path.has_value());
    const auto* patch = result->find_patch(fc::fixtures::simple_write::kPatchId);
    REQUIRE(patch != nullptr);
    CHECK(patch->state == fc::test::PatchState::Ready);
    REQUIRE(patch->operations.size() == 1);
    CHECK(patch->operations.front().kind == fc::test::OperationKind::Write);
    CHECK(patch->operations.front().has_evidence);
    CHECK(patch->operations.front().byte_size == sizeof(std::uint32_t));
}
