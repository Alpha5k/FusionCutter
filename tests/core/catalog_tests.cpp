#include "catalog.hpp"
#include "generated_catalog.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using fusioncutter::Architecture;
using fusioncutter::HostRole;
using fusioncutter::ImageTiming;
using fusioncutter::PatchDefinition;
using fusioncutter::PatchId;
using fusioncutter::PatchVariant;
using fusioncutter::PresentationCategory;
using fusioncutter::StartupFailurePolicy;
using fusioncutter::TargetContext;
using fusioncutter::TargetImage;
using fusioncutter::TargetLayout;
using fusioncutter::catalog::CatalogEntry;
using fusioncutter::catalog::CatalogScope;
using fusioncutter::catalog::PatchBuildEnvelope;

class FixturePatch final : public fusioncutter::Patch {
  public:
    explicit FixturePatch(const TargetContext&) {}
    void build_plan(fusioncutter::PatchPlan&) override {}
};

template <typename PatchType, typename Settings = fusioncutter::NoSettings>
[[nodiscard]] PatchVariant test_variant(TargetLayout layout, HostRole role, TargetImage image,
                                        ImageTiming image_timing = ImageTiming::Startup,
                                        StartupFailurePolicy failure_policy = StartupFailurePolicy::Local) {
    return {layout,       role,           image,
            image_timing, failure_policy, fusioncutter::patch_detail::patch_factory<PatchType, Settings>()};
}

struct TypedSettings {
    int value;
};

constexpr PresentationCategory kGameplay{"Gameplay", 200};
constexpr PatchBuildEnvelope kX86ClientEnvelope{true, false, true, false};
const PatchVariant kSteamClient =
    test_variant<FixturePatch>(TargetLayout::SteamRetail, HostRole::Client, TargetImage::Game);
const PatchVariant kGogClient =
    test_variant<FixturePatch>(TargetLayout::GOGRetail, HostRole::Client, TargetImage::Game);
const PatchVariant kGogLateClient = test_variant<FixturePatch>(TargetLayout::GOGRetail, HostRole::Client,
                                                               TargetImage::GalaxyPeer, ImageTiming::OneShotLate);
const PatchVariant kInvalidSecondSteamImage = test_variant<FixturePatch>(
    TargetLayout::SteamRetail, HostRole::Client, TargetImage::Bootstrap, ImageTiming::OneShotLate);
const PatchVariant kAspyrClient = test_variant<FixturePatch>(TargetLayout::Aspyr, HostRole::Client, TargetImage::Game);
const std::array kSteamVariants = {kSteamClient};
const std::array kGogVariants = {kGogClient};
const std::array kAspyrVariants = {kAspyrClient};

[[nodiscard]] PatchDefinition definition(bool enabled, std::span<const PatchId> dependencies = {},
                                         std::span<const PatchId> includes = {},
                                         std::span<const PatchVariant> variants = kSteamVariants,
                                         bool configurable = true, PresentationCategory category = kGameplay) {
    return {
        .name = "Test Patch",
        .enabled = enabled,
        .configurable = configurable,
        .category = category,
        .description = {},
        .settings = {},
        .depends_on = dependencies,
        .includes = includes,
        .variants = variants,
    };
}

[[nodiscard]] CatalogEntry entry(PatchId id, PatchDefinition patch_definition,
                                 PatchBuildEnvelope envelope = kX86ClientEnvelope) {
    return fusioncutter::catalog::catalog_entry(id, patch_definition, envelope);
}

[[nodiscard]] TargetContext steam_client_target() {
    return {
        TargetLayout::SteamRetail,
        HostRole::Client,
        {TargetImage::Game, Architecture::X86, 0x00400000, 0x00800000},
    };
}

[[nodiscard]] TargetContext gog_client_target() {
    auto target = steam_client_target();
    target.layout = TargetLayout::GOGRetail;
    return target;
}

[[nodiscard]] std::vector<std::string> selected_ids(const fusioncutter::catalog::PatchSelection& selection) {
    std::vector<std::string> ids;
    for (const auto& selected : selection.install_order) {
        ids.emplace_back(selected.entry->id);
    }
    return ids;
}

[[nodiscard]] std::vector<std::string> entry_ids(std::span<const CatalogEntry* const> entries) {
    std::vector<std::string> ids;
    for (const auto* catalog_entry : entries) {
        ids.emplace_back(catalog_entry->id);
    }
    return ids;
}

} // namespace

TEST_CASE("Catalog selection resolves required and automatic relationships in deterministic order", "[core][catalog]") {
    constexpr std::array<PatchId, 1> feature_requires = {"BaseSupport"};
    constexpr std::array<PatchId, 1> transport_selects = {"GalaxyPeerObserver"};
    constexpr std::array<PatchId, 1> observer_requires = {"DirectTransport"};

    auto catalog = fusioncutter::catalog::initialize_catalog(
        {
            entry("Unrelated", definition(true)),
            entry("GalaxyPeerObserver", definition(false, observer_requires, {}, kSteamVariants, false)),
            entry("Feature", definition(true, feature_requires)),
            entry("GogOnly", definition(true, {}, {}, kGogVariants)),
            entry("DirectTransport", definition(true, {}, transport_selects)),
            entry("BaseSupport", definition(false, {}, {}, kSteamVariants, false)),
        },
        CatalogScope{Architecture::X86, true, false});

    REQUIRE(catalog.has_value());
    CHECK(std::string(catalog->entries().front().id) == "BaseSupport");

    const auto selection = fusioncutter::catalog::select_patches(*catalog, steam_client_target());
    const std::vector<std::string> expected_order = {
        "BaseSupport", "DirectTransport", "Feature", "GalaxyPeerObserver", "Unrelated",
    };
    CHECK(selected_ids(selection) == expected_order);
    CHECK(selection.disabled.empty());
    CHECK(entry_ids(selection.not_applicable) == std::vector<std::string>{"GogOnly"});
}

TEST_CASE("Catalog selection includes a declared late image for the recognized environment", "[core][catalog]") {
    auto catalog = fusioncutter::catalog::initialize_catalog(
        {entry("GalaxyPeerObserver", definition(true, {}, {}, std::span{&kGogLateClient, 1}, false))},
        CatalogScope{Architecture::X86, true, false});
    REQUIRE(catalog.has_value());

    const auto selection = fusioncutter::catalog::select_patches(*catalog, gog_client_target());
    REQUIRE(selection.install_order.size() == 1);
    CHECK(selection.install_order.front().variant->image_timing == ImageTiming::OneShotLate);
    CHECK(selection.install_order.front().variant->image == TargetImage::GalaxyPeer);
}

TEST_CASE("Generated catalog discovers nested manifests in stable ID order", "[core][catalog]") {
    constexpr auto architecture = sizeof(void*) == 4 ? Architecture::X86 : Architecture::X64;
    auto entries = fusioncutter::catalog::generated_catalog_entries();
    constexpr bool includes_client_only = architecture == Architecture::X86 && FC_TEST_CATALOG_CLIENT != 0;
    REQUIRE(entries.size() == (includes_client_only ? 3 : 2));
    for (const auto& catalog_entry : entries) {
        CHECK(std::ranges::all_of(catalog_entry.definition.variants, [architecture](const auto& variant) {
            return fusioncutter::target_architecture(variant.layout) == architecture;
        }));
    }
    CHECK(std::string(entries[0].id) == "Alpha");
    if (includes_client_only) {
        CHECK(std::string(entries[1].id) == "ClientOnly");
    }
    CHECK(std::string(entries.back().id) == "Zulu");

    auto catalog = fusioncutter::catalog::initialize_catalog(
        std::move(entries), CatalogScope{architecture, FC_TEST_CATALOG_CLIENT != 0, FC_TEST_CATALOG_SERVER != 0});
    CHECK(catalog.has_value());
}

TEST_CASE("Explicit disable wins without removing unrelated selected patches", "[core][catalog]") {
    constexpr std::array<PatchId, 1> feature_requires = {"BaseSupport"};
    constexpr std::array<PatchId, 1> transport_selects = {"GalaxyPeerObserver"};
    constexpr std::array<PatchId, 1> observer_requires = {"DirectTransport"};

    auto catalog = fusioncutter::catalog::initialize_catalog(
        {
            entry("Feature", definition(true, feature_requires)),
            entry("BaseSupport", definition(false, {}, {}, kSteamVariants, false)),
            entry("DirectTransport", definition(true, {}, transport_selects)),
            entry("GalaxyPeerObserver", definition(false, observer_requires, {}, kSteamVariants, false)),
            entry("Unrelated", definition(true)),
        },
        CatalogScope{Architecture::X86, true, false});
    REQUIRE(catalog.has_value());

    constexpr std::array overrides = {
        fusioncutter::catalog::PatchOverride{"BaseSupport", false},
        fusioncutter::catalog::PatchOverride{"DirectTransport", false},
    };
    const auto selection = fusioncutter::catalog::select_patches(*catalog, steam_client_target(), overrides);

    CHECK(selected_ids(selection) == std::vector<std::string>{"Feature", "Unrelated"});
    CHECK(entry_ids(selection.disabled) ==
          std::vector<std::string>{"BaseSupport", "DirectTransport", "GalaxyPeerObserver"});
    CHECK(selection.not_applicable.empty());
}

TEST_CASE("Invalid catalog relationships and variants fail before selection", "[core][catalog]") {
    SECTION("missing reference") {
        constexpr std::array<PatchId, 1> missing = {"MissingPatch"};
        auto catalog = fusioncutter::catalog::initialize_catalog({entry("Dependent", definition(true, missing))},
                                                                 CatalogScope{Architecture::X86, true, false});

        REQUIRE_FALSE(catalog.has_value());
        CHECK(catalog.error().message.contains("does not exist"));
        REQUIRE(catalog.error().related_patch.has_value());
        CHECK(std::string(*catalog.error().related_patch) == "Dependent");
    }

    SECTION("required cycle") {
        constexpr std::array<PatchId, 1> first_requires = {"Second"};
        constexpr std::array<PatchId, 1> second_requires = {"First"};
        auto catalog = fusioncutter::catalog::initialize_catalog(
            {
                entry("First", definition(true, first_requires)),
                entry("Second", definition(true, second_requires)),
            },
            CatalogScope{Architecture::X86, true, false});

        REQUIRE_FALSE(catalog.has_value());
        CHECK(catalog.error().message.contains("cycle"));
    }

    SECTION("duplicate target tuple") {
        const std::array duplicate_variants = {kSteamClient, kSteamClient};
        auto catalog = fusioncutter::catalog::initialize_catalog(
            {entry("DuplicateVariant", definition(true, {}, {}, duplicate_variants))},
            CatalogScope{Architecture::X86, true, false});

        REQUIRE_FALSE(catalog.has_value());
        CHECK(catalog.error().message.contains("same target tuple"));
    }

    SECTION("multiple physical images for one environment") {
        const std::array variants = {kSteamClient, kInvalidSecondSteamImage};
        auto catalog =
            fusioncutter::catalog::initialize_catalog({entry("MultipleImages", definition(true, {}, {}, variants))},
                                                      CatalogScope{Architecture::X86, true, false});

        REQUIRE_FALSE(catalog.has_value());
        CHECK(catalog.error().message.contains("same layout and role"));
    }

    SECTION("variant outside manifest envelope") {
        auto catalog = fusioncutter::catalog::initialize_catalog(
            {entry("WrongEnvelope", definition(true, {}, {}, kAspyrVariants))},
            CatalogScope{Architecture::X86, true, false});

        REQUIRE_FALSE(catalog.has_value());
        CHECK(catalog.error().message.contains("exceeds"));
    }

    SECTION("startup-required late image") {
        const std::array late_variant = {test_variant<FixturePatch>(TargetLayout::SteamRetail, HostRole::Client,
                                                                    TargetImage::GalaxyPeer, ImageTiming::OneShotLate,
                                                                    StartupFailurePolicy::StartupRequired)};
        auto catalog =
            fusioncutter::catalog::initialize_catalog({entry("LateRequired", definition(true, {}, {}, late_variant))},
                                                      CatalogScope{Architecture::X86, true, false});

        REQUIRE_FALSE(catalog.has_value());
        CHECK(catalog.error().message.contains("cannot be startup-required"));
    }

    SECTION("category order disagreement") {
        auto catalog = fusioncutter::catalog::initialize_catalog(
            {
                entry("First", definition(true, {}, {}, kSteamVariants, true, {"Gameplay", 100})),
                entry("Second", definition(true, {}, {}, kSteamVariants, true, {"Gameplay", 200})),
            },
            CatalogScope{Architecture::X86, true, false});

        REQUIRE_FALSE(catalog.has_value());
        CHECK(catalog.error().message.contains("category order"));
    }

    SECTION("factory settings type disagreement") {
        auto patch_definition = definition(true);
        patch_definition.settings = fusioncutter::SettingsDefinition::from(fusioncutter::SettingsSchema<TypedSettings>{
            .values = {fusioncutter::setting("Value", &TypedSettings::value, 1)},
        });
        auto catalog = fusioncutter::catalog::initialize_catalog({entry("WrongFactory", std::move(patch_definition))},
                                                                 CatalogScope{Architecture::X86, true, false});

        REQUIRE_FALSE(catalog.has_value());
        CHECK(catalog.error().message.contains("wrong settings type"));
    }

    SECTION("invalid settings default") {
        auto patch_definition = definition(true);
        patch_definition.settings = fusioncutter::SettingsDefinition::from(fusioncutter::SettingsSchema<TypedSettings>{
            .values = {fusioncutter::setting("Value", &TypedSettings::value, 10).range(0, 5)},
        });
        auto catalog = fusioncutter::catalog::initialize_catalog({entry("WrongDefault", std::move(patch_definition))},
                                                                 CatalogScope{Architecture::X86, true, false});

        REQUIRE_FALSE(catalog.has_value());
        CHECK(catalog.error().message.contains("invalid range or default"));
    }
}
