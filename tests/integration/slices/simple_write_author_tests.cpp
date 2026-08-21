#include "simple_write.hpp"
#include "synthetic_image.hpp"

#include <FusionCutter/Testing.hpp>

#include <catch2/catch_test_macros.hpp>

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

// Every Scenario gets private source/configuration storage so generation and override checks cannot affect one another.
class TemporaryDirectory final {
  public:
    TemporaryDirectory() {
        static std::atomic_uint64_t sequence{};
        path = std::filesystem::temp_directory_path() /
               ("FusionCutter-SimpleWrite-" + std::to_string(GetCurrentProcessId()) + "-" +
                std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)));
        REQUIRE(std::filesystem::create_directory(path));
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }

    std::filesystem::path path;
};

[[nodiscard]] std::filesystem::path plugin_path() {
    return std::filesystem::current_path() / "plugins" / "fc_simple_write_slice.dll";
}

[[nodiscard]] std::span<const std::byte> value_bytes(std::uint32_t value,
                                                     std::array<std::byte, sizeof(value)>& storage) noexcept {
    std::memcpy(storage.data(), &value, sizeof(value));
    return storage;
}

// Keeps Scenario's borrowed loaded-layout images alive through validation.
struct ScenarioInput {
    fc::test::Scenario scenario;
    std::vector<std::vector<std::byte>> images;
};

// Selects one real production tuple per architecture while supplying only synthetic loaded-layout image bytes.
[[nodiscard]] ScenarioInput scenario_with_value(std::uint32_t value) {
    std::array<std::byte, sizeof(value)> payload{};
#if defined(_M_IX86)
    const fc::TargetInfo target{fc::TargetLayout::SteamRetail, fc::HostRole::Client, fc::Architecture::X86,
                                "SteamRetail_Game_59EDE353"};
    ScenarioInput result{fc::test::Scenario{target.layout, target.role}};
    result.images.push_back(fc::fixtures::synthetic_image(
        target.architecture, 0x400000, fc::fixtures::simple_write::value_rva(target), value_bytes(value, payload)));
    result.scenario.add_image(fc::TargetImage::Game, std::string{target.image_profile}, result.images.back());
#else
    const fc::TargetInfo target{fc::TargetLayout::ClassicCollection, fc::HostRole::Client, fc::Architecture::X64,
                                "ClassicCollection_Game_66702CD2"};
    ScenarioInput result{fc::test::Scenario{target.layout, target.role}};
    result.images.push_back(
        fc::fixtures::synthetic_image(target.architecture, 0x2000, {0x1000}, std::span<const std::byte>{}));
    result.scenario.add_image(fc::TargetImage::Bootstrap, "ClassicCollection_Bootstrap_66702CD7", result.images.back());
    result.images.push_back(fc::fixtures::synthetic_image(
        target.architecture, 0x700000, fc::fixtures::simple_write::value_rva(target), value_bytes(value, payload)));
    result.scenario.add_image(fc::TargetImage::Game, std::string{target.image_profile}, result.images.back());
#endif
    result.scenario.add_plugin(plugin_path());
    return result;
}

void write_override(const std::filesystem::path& directory) {
    REQUIRE(std::filesystem::create_directory(directory / "config"));
    std::ofstream output{directory / "config" / "FC.SimpleWriteSlice.ini", std::ios::binary};
    REQUIRE(output << "[General]\r\nSimpleCheckedWrite=true\r\n\r\n[SimpleCheckedWrite]\r\nReplacement="
                   << fc::fixtures::simple_write::kOverrideReplacement << "\r\n");
}

} // namespace

TEST_CASE("simple checked-write Scenario validates external metadata without mutating its image") {
    auto input = scenario_with_value(fc::fixtures::simple_write::kInitialValue);
    const auto before = input.images;
    const auto result = input.scenario.validate();
    REQUIRE(result);

    // Structured results prove real DLL admission and the one evidence-bearing write without parsing diagnostics.
    const auto* plugin = result->find_plugin(fc::fixtures::simple_write::kPluginId);
    REQUIRE(plugin != nullptr);
    CHECK(plugin->admitted);
    const auto* patch = result->find_patch(fc::fixtures::simple_write::kPatchId);
    REQUIRE(patch != nullptr);
    CHECK(patch->state == fc::test::PatchState::Ready);
    REQUIRE(patch->operations.size() == 1);
    CHECK(patch->operations.front().kind == fc::test::OperationKind::Write);
    CHECK(patch->operations.front().has_evidence);
    CHECK(patch->operations.front().byte_size == sizeof(std::uint32_t));

    // Exact byte comparison proves the public testing API stopped before installation for every supplied image.
    REQUIRE(input.images.size() == before.size());
    for (std::size_t index = 0; index < input.images.size(); ++index) {
        REQUIRE(input.images[index].size() == before[index].size());
        CHECK(std::memcmp(input.images[index].data(), before[index].data(), before[index].size()) == 0);
    }
}

TEST_CASE("simple checked-write Scenario applies private configuration and rejects changed evidence") {
    TemporaryDirectory configuration;
    write_override(configuration.path);

    auto accepted = scenario_with_value(fc::fixtures::simple_write::kInitialValue);
    accepted.scenario.use_config(configuration.path / "config");
    const auto configured = accepted.scenario.validate();
    REQUIRE(configured);
    const auto* configured_patch = configured->find_patch(fc::fixtures::simple_write::kPatchId);
    REQUIRE(configured_patch != nullptr);
    CHECK(configured_patch->state == fc::test::PatchState::Ready);

    // Changing only the expected value isolates the evidence failure on the same production validation path.
    auto changed = scenario_with_value(fc::fixtures::simple_write::kInitialValue ^ 1U);
    const auto rejected = changed.scenario.validate();
    REQUIRE(rejected);
    const auto* patch = rejected->find_patch(fc::fixtures::simple_write::kPatchId);
    REQUIRE(patch != nullptr);
    CHECK(patch->state == fc::test::PatchState::Failed);
    CHECK(patch->phase == fc::test::PatchPhase::Plan);
}
