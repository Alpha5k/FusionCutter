#include "relocated_plan.hpp"
#include "synthetic_image.hpp"

#include <FusionCutter/Testing.hpp>

#include <catch2/catch_test_macros.hpp>

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ranges>
#include <string>
#include <vector>

namespace {

// Configuration variants are copied by Scenario, leaving these source fixtures unchanged and reusable.
class TemporaryDirectory final {
  public:
    TemporaryDirectory() {
        static std::atomic_uint64_t sequence{};
        path = std::filesystem::temp_directory_path() /
               ("FusionCutter-RelocatedPlan-" + std::to_string(GetCurrentProcessId()) + "-" +
                std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)));
        REQUIRE(std::filesystem::create_directory(path));
        REQUIRE(std::filesystem::create_directory(path / "config"));
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }

    std::filesystem::path path;
};

// Keeps Scenario's borrowed loaded-layout images alive through validation.
struct ScenarioInput {
    fc::test::Scenario scenario;
    std::vector<std::vector<std::byte>> images;
};

// Builds one coherent production tuple whose writable, executable, and read-only sections exercise distinct claims.
[[nodiscard]] ScenarioInput relocated_scenario() {
#if defined(_M_IX86)
    const fc::TargetInfo target{fc::TargetLayout::SteamRetail, fc::HostRole::Client, fc::Architecture::X86,
                                "SteamRetail_Game_59EDE353"};
    constexpr std::uint32_t image_size = 0x400000;
    constexpr fc::Rva rdata{0x36b000};
    constexpr fc::Rva data{0x3de000};
#else
    const fc::TargetInfo target{fc::TargetLayout::ClassicCollection, fc::HostRole::Client, fc::Architecture::X64,
                                "ClassicCollection_Game_66702CD2"};
    constexpr std::uint32_t image_size = 0x650000;
    constexpr fc::Rva rdata{0x501000};
    constexpr fc::Rva data{0x639000};
#endif
    const std::array sections{
        fc::fixtures::SyntheticSection{
            ".text", {0x1000}, 0x4000, IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ},
        fc::fixtures::SyntheticSection{".rdata", rdata, 0x2000, IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ},
        fc::fixtures::SyntheticSection{".data", data, 0x5000,
                                       IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE},
    };
    ScenarioInput result{fc::test::Scenario{target.layout, target.role}};
#if defined(_M_X64)
    // Classic Collection scenarios require the reviewed bootstrap alongside the patchable game image.
    result.images.push_back(
        fc::fixtures::synthetic_image(target.architecture, 0x2000, {0x1000}, std::span<const std::byte>{}));
    result.scenario.add_image(fc::TargetImage::Bootstrap, "ClassicCollection_Bootstrap_66702CD7", result.images.back());
#endif
    // Scenario borrows loaded-layout image bytes, so the owning vectors remain beside it for the validation call.
    result.images.push_back(fc::fixtures::synthetic_image(target.architecture, image_size, sections));
    result.scenario.add_image(fc::TargetImage::Game, std::string{target.image_profile}, result.images.back());
    result.scenario.add_plugin(std::filesystem::current_path() / "plugins" / "fc_relocated_plan_slice.dll");
    return result;
}

void write_configuration(const std::filesystem::path& directory, std::string_view content) {
    std::ofstream output{directory / "config" / "FC.RelocatedPlanSlice.ini", std::ios::binary};
    REQUIRE(output << content);
}

[[nodiscard]] std::size_t operation_count(const fc::test::PatchResult& patch, fc::test::OperationKind kind) {
    return static_cast<std::size_t>(std::ranges::count(patch.operations, kind, &fc::test::Operation::kind));
}

} // namespace

TEST_CASE("relocated plan preserves target rows, symbolic operations, and conflict isolation") {
    TemporaryDirectory configuration;
    write_configuration(configuration.path, "[Memory]\r\nRelocatedFeatures=true\r\nRelocatedConflictA=true\r\n"
                                            "RelocatedConflictB=true\r\n\r\n[RelocatedStorage]\r\nAllocationCount=6\r\n"
                                            "UseMutableLocation=true\r\n\r\n[RelocatedIndependent]\r\n"
                                            "Replacement=287454020\r\n");
    auto input = relocated_scenario();
    input.scenario.use_config(configuration.path / "config");
    const auto result = input.scenario.validate();
    REQUIRE(result);
    REQUIRE(result->find_plugin(fc::fixtures::relocated_plan::kPluginId) != nullptr);

    // The storage patch proves every representative operation family survives the common validation path.
    const auto* storage = result->find_patch(fc::fixtures::relocated_plan::kStoragePatchId);
    REQUIRE(storage != nullptr);
    CHECK(storage->state == fc::test::PatchState::Ready);
    CHECK(operation_count(*storage, fc::test::OperationKind::AllocateData) == 1);
    CHECK(operation_count(*storage, fc::test::OperationKind::Write) == 3);
    CHECK(operation_count(*storage, fc::test::OperationKind::Require) == 3);
    CHECK(storage->claims.size() >= 6);

    // Both overlapping writers fail while the configured compact patch retains an independent ready result.
    for (const auto id :
         {fc::fixtures::relocated_plan::kConflictAPatchId, fc::fixtures::relocated_plan::kConflictBPatchId}) {
        const auto* conflict = result->find_patch(id);
        REQUIRE(conflict != nullptr);
        CHECK(conflict->state == fc::test::PatchState::Failed);
        CHECK(conflict->phase == fc::test::PatchPhase::Validation);
    }
    const auto* independent = result->find_patch(fc::fixtures::relocated_plan::kIndependentPatchId);
    REQUIRE(independent != nullptr);
    CHECK(independent->state == fc::test::PatchState::Ready);
    CHECK(operation_count(*independent, fc::test::OperationKind::Write) == 1);
}

TEST_CASE("relocated configurable group controls its nonconfigurable members") {
    TemporaryDirectory configuration;
    write_configuration(configuration.path, "[Memory]\r\nRelocatedFeatures=false\r\n");
    auto input = relocated_scenario();
    input.scenario.use_config(configuration.path / "config");
    const auto result = input.scenario.validate();
    REQUIRE(result);
    const auto* storage = result->find_patch(fc::fixtures::relocated_plan::kStoragePatchId);
    const auto* independent = result->find_patch(fc::fixtures::relocated_plan::kIndependentPatchId);
    REQUIRE(storage != nullptr);
    REQUIRE(independent != nullptr);
    CHECK(storage->state == fc::test::PatchState::Disabled);
    CHECK(independent->state == fc::test::PatchState::Disabled);
}
