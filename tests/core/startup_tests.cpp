#include "catalog.hpp"
#include "configuration.hpp"
#include "startup.hpp"

#include <FusionCutter/patch.hpp>

#include <Windows.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

#if defined(_M_IX86)
constexpr auto kArchitecture = fusioncutter::Architecture::X86;
constexpr auto kLayout = fusioncutter::TargetLayout::SteamRetail;
#else
constexpr auto kArchitecture = fusioncutter::Architecture::X64;
constexpr auto kLayout = fusioncutter::TargetLayout::Aspyr;
#endif

class ImagePage {
  public:
    ImagePage() {
        data_ = static_cast<std::byte*>(VirtualAlloc(nullptr, kSize, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        REQUIRE(data_ != nullptr);
    }

    ImagePage(const ImagePage&) = delete;
    ImagePage& operator=(const ImagePage&) = delete;

    ~ImagePage() {
        if (data_ != nullptr) {
            VirtualFree(data_, 0, MEM_RELEASE);
        }
    }

    [[nodiscard]] std::byte& at(std::uint32_t rva) noexcept {
        return data_[rva];
    }

    [[nodiscard]] fusioncutter::TargetContext target(fusioncutter::TargetImage image = fusioncutter::TargetImage::Game,
                                                     fusioncutter::TargetLayout layout = kLayout) const noexcept {
        return {layout,
                fusioncutter::HostRole::Client,
                {image, kArchitecture, reinterpret_cast<std::uintptr_t>(data_), kSize}};
    }

  private:
    static constexpr std::size_t kSize = 4096;
    std::byte* data_{};
};

struct Behavior {
    std::string_view name;
    std::vector<std::string>* events{};
    std::byte* memory{};
    std::uint32_t rva{};
    std::byte expected{};
    std::byte replacement{};
    bool fail_prepare{};
    bool prepared_before_write{};
    bool enabled_after_write{};
    int updates{};
};

template <std::size_t Index> Behavior g_behavior;

template <std::size_t Index>
class RuntimeFixture final : public fusioncutter::RuntimePatch, public fusioncutter::Updatable {
  public:
    explicit RuntimeFixture(const fusioncutter::TargetContext&) {}

    void build_plan(fusioncutter::PatchPlan& plan) override {
        auto& behavior = g_behavior<Index>;
        behavior.events->push_back("build:" + std::string(behavior.name));
        if (behavior.memory == nullptr) {
            return;
        }
        const std::array expected{behavior.expected};
        const std::array replacement{behavior.replacement};
        plan.checked_write(behavior.name, behavior.rva, fusioncutter::BytePattern::exact(expected), replacement);
    }

    [[nodiscard]] std::expected<void, fusioncutter::OutcomeReason> prepare_runtime() override {
        auto& behavior = g_behavior<Index>;
        behavior.events->push_back("prepare:" + std::string(behavior.name));
        behavior.prepared_before_write =
            behavior.memory == nullptr || behavior.memory[behavior.rva] == behavior.expected;
        if (behavior.fail_prepare) {
            return std::unexpected(fusioncutter::OutcomeReason{"fixture runtime preparation failed", {}, {}});
        }
        return {};
    }

    void enable_runtime() noexcept override {
        auto& behavior = g_behavior<Index>;
        behavior.events->push_back("enable:" + std::string(behavior.name));
        behavior.enabled_after_write =
            behavior.memory == nullptr || behavior.memory[behavior.rva] == behavior.replacement;
    }

    void update() noexcept override {
        ++g_behavior<Index>.updates;
    }
};

template <std::size_t Index>
class RuntimeOnlyFixture final : public fusioncutter::RuntimeOnlyPatch, public fusioncutter::Updatable {
  public:
    explicit RuntimeOnlyFixture(const fusioncutter::TargetContext&) {}

    [[nodiscard]] std::expected<void, fusioncutter::OutcomeReason> prepare_runtime() override {
        g_behavior<Index>.events->push_back("prepare:" + std::string(g_behavior<Index>.name));
        return {};
    }

    void enable_runtime() noexcept override {
        g_behavior<Index>.events->push_back("enable:" + std::string(g_behavior<Index>.name));
    }

    void update() noexcept override {
        ++g_behavior<Index>.updates;
    }
};

class StatusRuntimeFixture final : public fusioncutter::RuntimeOnlyPatch, public fusioncutter::StatusContributor {
  public:
    explicit StatusRuntimeFixture(const fusioncutter::TargetContext&) {}

    void write_status(fusioncutter::StatusSection&) const noexcept override {}
};

class TemporaryDirectory {
  public:
    TemporaryDirectory() {
        static std::atomic_uint64_t sequence;
        path_ = std::filesystem::temp_directory_path() /
                ("FusionCutter-startup-tests-" + std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)));
        std::filesystem::create_directories(path_);
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] std::filesystem::path file(std::string_view name) const {
        return path_ / name;
    }

  private:
    std::filesystem::path path_;
};

void write_file(const std::filesystem::path& path, std::string_view content) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    REQUIRE(output);
    output.write(content.data(), static_cast<std::streamsize>(content.size()));
    REQUIRE(output);
}

[[nodiscard]] fusioncutter::PatchDefinition definition(bool enabled,
                                                       std::span<const fusioncutter::PatchVariant> variants,
                                                       std::span<const fusioncutter::PatchId> dependencies = {},
                                                       bool configurable = false) {
    return {
        .name = "Test Patch",
        .enabled = enabled,
        .configurable = configurable,
        .category = {"Tests", 10},
        .description = {},
        .settings = {},
        .depends_on = dependencies,
        .includes = {},
        .variants = variants,
    };
}

[[nodiscard]] fusioncutter::catalog::CatalogEntry entry(fusioncutter::PatchId id,
                                                        fusioncutter::PatchDefinition patch_definition) {
    return fusioncutter::catalog::catalog_entry(id, std::move(patch_definition), {true, true, true, false});
}

[[nodiscard]] const fusioncutter::PatchResult& result(const fusioncutter::StartupState& state,
                                                      fusioncutter::PatchId patch_id) {
    const auto results = state.patch_results();
    const auto found = std::ranges::find(results, patch_id, &fusioncutter::PatchResult::patch_id);
    REQUIRE(found != results.end());
    return *found;
}

template <std::size_t Index> void reset_behavior(Behavior behavior) {
    g_behavior<Index> = behavior;
}

#if defined(_M_IX86)
std::optional<fusioncutter::TargetContext> g_late_target;

[[nodiscard]] std::expected<std::optional<fusioncutter::TargetContext>, fusioncutter::OutcomeReason>
late_probe(fusioncutter::TargetLayout, fusioncutter::HostRole, fusioncutter::TargetImage) {
    return g_late_target;
}
#endif

} // namespace

TEST_CASE("Startup validates every plan before runtime preparation and commits before enable", "[core][startup]") {
    ImagePage image;
    std::vector<std::string> events;
    image.at(16) = std::byte{0x10};
    image.at(32) = std::byte{0x20};
    reset_behavior<0>({"Base", &events, &image.at(0), 16, std::byte{0x10}, std::byte{0x11}});
    reset_behavior<1>({"Feature", &events, &image.at(0), 32, std::byte{0x20}, std::byte{0x21}});

    const fusioncutter::PatchVariants base_variants{fusioncutter::make_patch_variant<RuntimeFixture<0>, kLayout>(
        fusioncutter::HostRole::Client, fusioncutter::TargetImage::Game)};
    const fusioncutter::PatchVariants feature_variants{fusioncutter::make_patch_variant<RuntimeFixture<1>, kLayout>(
        fusioncutter::HostRole::Client, fusioncutter::TargetImage::Game)};
    constexpr std::array<fusioncutter::PatchId, 1> feature_requires = {"Base"};
    auto catalog = fusioncutter::catalog::initialize_catalog(
        {entry("Feature", definition(true, feature_variants, feature_requires)),
         entry("Base", definition(true, base_variants))},
        {kArchitecture, true, false});
    REQUIRE(catalog.has_value());

    const auto target = image.target();
    auto state = fusioncutter::run_startup(std::move(*catalog), {}, std::span{&target, 1});

    CHECK(state.initialization_result().outcome == fusioncutter::InitializationOutcome::Completed);
    CHECK(result(state, "Base").outcome == fusioncutter::PatchOutcome::Installed);
    CHECK(result(state, "Feature").outcome == fusioncutter::PatchOutcome::Installed);
    CHECK(g_behavior<0>.prepared_before_write);
    CHECK(g_behavior<1>.prepared_before_write);
    CHECK(g_behavior<0>.enabled_after_write);
    CHECK(g_behavior<1>.enabled_after_write);
    CHECK(std::to_integer<unsigned int>(image.at(16)) == 0x11);
    CHECK(std::to_integer<unsigned int>(image.at(32)) == 0x21);

    const auto first_prepare = std::ranges::find_if(events, [](const auto& event) {
        return event.starts_with("prepare:");
    });
    REQUIRE(first_prepare != events.end());
    const auto build_count = static_cast<std::size_t>(first_prepare - events.begin());
    CHECK(std::ranges::all_of(std::span{events.data(), build_count}, [](const auto& event) {
        return event.starts_with("build:");
    }));

    state.update(nullptr);
    CHECK(g_behavior<0>.updates == 1);
    CHECK(g_behavior<1>.updates == 1);
}

TEST_CASE("Startup discovers optional live status contributors once", "[core][startup]") {
    ImagePage image;
    const fusioncutter::PatchVariants variants{fusioncutter::make_patch_variant<StatusRuntimeFixture, kLayout>(
        fusioncutter::HostRole::Client, fusioncutter::TargetImage::Game)};
    auto catalog = fusioncutter::catalog::initialize_catalog({entry("LiveFeature", definition(true, variants))},
                                                             {kArchitecture, true, false});
    REQUIRE(catalog.has_value());

    const auto target = image.target();
    auto state = fusioncutter::run_startup(std::move(*catalog), {}, std::span{&target, 1});
    REQUIRE(state.status_contributors().size() == 1);
    CHECK(std::string(state.status_contributors().front().name) == "Test Patch");
    CHECK(state.status_contributors().front().contributor != nullptr);
}

TEST_CASE("A local plan failure skips dependents and preserves unrelated installation", "[core][startup]") {
    ImagePage image;
    std::vector<std::string> events;
    image.at(16) = std::byte{0x10};
    image.at(48) = std::byte{0x30};
    reset_behavior<0>({"Broken", &events, &image.at(0), 16, std::byte{0x7F}, std::byte{0x11}});
    reset_behavior<1>({"Dependent", &events});
    reset_behavior<2>({"Unrelated", &events, &image.at(0), 48, std::byte{0x30}, std::byte{0x31}});

    const fusioncutter::PatchVariants broken_variants{fusioncutter::make_patch_variant<RuntimeFixture<0>, kLayout>(
        fusioncutter::HostRole::Client, fusioncutter::TargetImage::Game)};
    const fusioncutter::PatchVariants dependent_variants{fusioncutter::make_patch_variant<RuntimeFixture<1>, kLayout>(
        fusioncutter::HostRole::Client, fusioncutter::TargetImage::Game)};
    const fusioncutter::PatchVariants unrelated_variants{fusioncutter::make_patch_variant<RuntimeFixture<2>, kLayout>(
        fusioncutter::HostRole::Client, fusioncutter::TargetImage::Game)};
    constexpr std::array<fusioncutter::PatchId, 1> dependent_requires = {"Broken"};
    auto catalog = fusioncutter::catalog::initialize_catalog(
        {entry("Broken", definition(true, broken_variants)),
         entry("Dependent", definition(true, dependent_variants, dependent_requires)),
         entry("Unrelated", definition(true, unrelated_variants))},
        {kArchitecture, true, false});
    REQUIRE(catalog.has_value());

    const auto target = image.target();
    auto state = fusioncutter::run_startup(std::move(*catalog), {}, std::span{&target, 1});

    CHECK(state.initialization_result().outcome == fusioncutter::InitializationOutcome::Completed);
    CHECK(result(state, "Broken").outcome == fusioncutter::PatchOutcome::Failed);
    CHECK(result(state, "Dependent").outcome == fusioncutter::PatchOutcome::Skipped);
    REQUIRE(result(state, "Dependent").reason.has_value());
    CHECK(result(state, "Dependent").reason->related_patch == "Broken");
    CHECK(result(state, "Unrelated").outcome == fusioncutter::PatchOutcome::Installed);
    CHECK(std::to_integer<unsigned int>(image.at(16)) == 0x10);
    CHECK(std::to_integer<unsigned int>(image.at(48)) == 0x31);
}

TEST_CASE("Runtime preparation failure writes nothing for that patch and unrelated work continues", "[core][startup]") {
    ImagePage image;
    std::vector<std::string> events;
    image.at(16) = std::byte{0x10};
    image.at(48) = std::byte{0x30};
    reset_behavior<0>({"Service", &events, &image.at(0), 16, std::byte{0x10}, std::byte{0x11}, true});
    reset_behavior<1>({"Unrelated", &events, &image.at(0), 48, std::byte{0x30}, std::byte{0x31}});

    const fusioncutter::PatchVariants service_variants{fusioncutter::make_patch_variant<RuntimeFixture<0>, kLayout>(
        fusioncutter::HostRole::Client, fusioncutter::TargetImage::Game)};
    const fusioncutter::PatchVariants unrelated_variants{fusioncutter::make_patch_variant<RuntimeFixture<1>, kLayout>(
        fusioncutter::HostRole::Client, fusioncutter::TargetImage::Game)};
    auto catalog = fusioncutter::catalog::initialize_catalog({entry("Service", definition(true, service_variants)),
                                                              entry("Unrelated", definition(true, unrelated_variants))},
                                                             {kArchitecture, true, false});
    REQUIRE(catalog.has_value());

    const auto target = image.target();
    auto state = fusioncutter::run_startup(std::move(*catalog), {}, std::span{&target, 1});

    CHECK(state.initialization_result().outcome == fusioncutter::InitializationOutcome::Completed);
    CHECK(result(state, "Service").outcome == fusioncutter::PatchOutcome::Failed);
    CHECK(result(state, "Unrelated").outcome == fusioncutter::PatchOutcome::Installed);
    CHECK(std::to_integer<unsigned int>(image.at(16)) == 0x10);
    CHECK(std::to_integer<unsigned int>(image.at(48)) == 0x31);
}

TEST_CASE("An invalid patch toggle fails only that patch", "[core][startup]") {
    ImagePage image;
    std::vector<std::string> events;
    reset_behavior<0>({"Configured", &events});
    reset_behavior<1>({"Unrelated", &events});

    const fusioncutter::PatchVariants configured_variants{fusioncutter::make_patch_variant<RuntimeFixture<0>, kLayout>(
        fusioncutter::HostRole::Client, fusioncutter::TargetImage::Game)};
    const fusioncutter::PatchVariants unrelated_variants{fusioncutter::make_patch_variant<RuntimeFixture<1>, kLayout>(
        fusioncutter::HostRole::Client, fusioncutter::TargetImage::Game)};
    auto catalog =
        fusioncutter::catalog::initialize_catalog({entry("Configured", definition(true, configured_variants, {}, true)),
                                                   entry("Unrelated", definition(true, unrelated_variants))},
                                                  {kArchitecture, true, false});
    REQUIRE(catalog.has_value());

    TemporaryDirectory directory;
    const auto path = directory.file("FusionCutter.ini");
    write_file(path, "[Patches]\r\nConfigured=maybe\r\n");
    const auto target = image.target();
    const auto applicable = fusioncutter::catalog::configurable_patches(*catalog, target);
    auto configuration = fusioncutter::config::load_configuration(path, applicable);
    REQUIRE(configuration.has_value());

    auto state = fusioncutter::run_startup(std::move(*catalog), std::move(*configuration), std::span{&target, 1});

    CHECK(state.initialization_result().outcome == fusioncutter::InitializationOutcome::Completed);
    CHECK(result(state, "Configured").outcome == fusioncutter::PatchOutcome::Failed);
    CHECK(result(state, "Unrelated").outcome == fusioncutter::PatchOutcome::Installed);
}

TEST_CASE("Startup-required policy promotes patch and dependency failures to fatal", "[core][startup]") {
    SECTION("patch preparation failure") {
        ImagePage image;
        std::vector<std::string> events;
        image.at(16) = std::byte{0x10};
        image.at(48) = std::byte{0x30};
        reset_behavior<0>({"Critical", &events, &image.at(0), 16, std::byte{0x10}, std::byte{0x11}, true});
        reset_behavior<1>({"Unrelated", &events, &image.at(0), 48, std::byte{0x30}, std::byte{0x31}});

        const fusioncutter::PatchVariants critical_variants{
            fusioncutter::make_patch_variant<RuntimeFixture<0>, kLayout>(
                fusioncutter::HostRole::Client, fusioncutter::TargetImage::Game, fusioncutter::ImageTiming::Startup,
                fusioncutter::StartupFailurePolicy::StartupRequired)};
        const fusioncutter::PatchVariants unrelated_variants{
            fusioncutter::make_patch_variant<RuntimeFixture<1>, kLayout>(fusioncutter::HostRole::Client,
                                                                         fusioncutter::TargetImage::Game)};
        auto catalog =
            fusioncutter::catalog::initialize_catalog({entry("Critical", definition(true, critical_variants)),
                                                       entry("Unrelated", definition(true, unrelated_variants))},
                                                      {kArchitecture, true, false});
        REQUIRE(catalog.has_value());

        const auto target = image.target();
        auto state = fusioncutter::run_startup(std::move(*catalog), {}, std::span{&target, 1});

        CHECK(state.initialization_result().outcome == fusioncutter::InitializationOutcome::Fatal);
        CHECK(result(state, "Critical").outcome == fusioncutter::PatchOutcome::Failed);
        CHECK(result(state, "Unrelated").outcome == fusioncutter::PatchOutcome::Skipped);
        CHECK(std::to_integer<unsigned int>(image.at(16)) == 0x10);
        CHECK(std::to_integer<unsigned int>(image.at(48)) == 0x30);
    }

    SECTION("explicitly disabled dependency") {
        ImagePage image;
        std::vector<std::string> events;
        reset_behavior<0>({"Critical", &events});
        reset_behavior<1>({"Support", &events});

        const fusioncutter::PatchVariants critical_variants{
            fusioncutter::make_patch_variant<RuntimeFixture<0>, kLayout>(
                fusioncutter::HostRole::Client, fusioncutter::TargetImage::Game, fusioncutter::ImageTiming::Startup,
                fusioncutter::StartupFailurePolicy::StartupRequired)};
        const fusioncutter::PatchVariants support_variants{fusioncutter::make_patch_variant<RuntimeFixture<1>, kLayout>(
            fusioncutter::HostRole::Client, fusioncutter::TargetImage::Game)};
        constexpr std::array<fusioncutter::PatchId, 1> critical_requires = {"Support"};
        auto catalog = fusioncutter::catalog::initialize_catalog(
            {entry("Critical", definition(true, critical_variants, critical_requires)),
             entry("Support", definition(true, support_variants, {}, true))},
            {kArchitecture, true, false});
        REQUIRE(catalog.has_value());

        TemporaryDirectory directory;
        const auto path = directory.file("FusionCutter.ini");
        write_file(path, "[Patches]\r\nSupport=0\r\n");
        const auto target = image.target();
        const auto applicable = fusioncutter::catalog::configurable_patches(*catalog, target);
        auto configuration = fusioncutter::config::load_configuration(path, applicable);
        REQUIRE(configuration.has_value());

        auto state = fusioncutter::run_startup(std::move(*catalog), std::move(*configuration), std::span{&target, 1});

        CHECK(state.initialization_result().outcome == fusioncutter::InitializationOutcome::Fatal);
        CHECK(result(state, "Support").outcome == fusioncutter::PatchOutcome::Disabled);
        CHECK(result(state, "Critical").outcome == fusioncutter::PatchOutcome::Skipped);
        REQUIRE(result(state, "Critical").reason.has_value());
        CHECK(result(state, "Critical").reason->related_patch == "Support");
    }
}

#if defined(_M_IX86)
TEST_CASE("A selected GOG late-image patch transitions once from waiting to installed", "[core][startup]") {
    ImagePage startup_image;
    ImagePage late_image;
    std::vector<std::string> events;
    reset_behavior<0>({"Late", &events});
    g_late_target.reset();

    const fusioncutter::PatchVariants variants{
        fusioncutter::make_patch_variant<RuntimeOnlyFixture<0>, fusioncutter::TargetLayout::GOGRetail>(
            fusioncutter::HostRole::Client, fusioncutter::TargetImage::GalaxyPeer,
            fusioncutter::ImageTiming::OneShotLate)};
    auto catalog = fusioncutter::catalog::initialize_catalog({entry("Late", definition(true, variants))},
                                                             {fusioncutter::Architecture::X86, true, false});
    REQUIRE(catalog.has_value());

    const auto startup_target =
        startup_image.target(fusioncutter::TargetImage::Game, fusioncutter::TargetLayout::GOGRetail);
    auto state = fusioncutter::run_startup(std::move(*catalog), {}, std::span{&startup_target, 1});
    CHECK(result(state, "Late").outcome == fusioncutter::PatchOutcome::WaitingForImage);

    state.update(late_probe);
    CHECK(result(state, "Late").outcome == fusioncutter::PatchOutcome::WaitingForImage);
    CHECK(g_behavior<0>.updates == 0);

    g_late_target = late_image.target(fusioncutter::TargetImage::GalaxyPeer, fusioncutter::TargetLayout::GOGRetail);
    state.update(late_probe);
    CHECK(result(state, "Late").outcome == fusioncutter::PatchOutcome::Installed);
    CHECK(g_behavior<0>.updates == 1);

    g_late_target.reset();
    state.update(late_probe);
    CHECK(result(state, "Late").outcome == fusioncutter::PatchOutcome::Installed);
    CHECK(g_behavior<0>.updates == 2);
}
#endif
