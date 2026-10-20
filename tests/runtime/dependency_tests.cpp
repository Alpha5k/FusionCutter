#include "patches/dependency.hpp"
#include "patches/patch_record.hpp"

#include <fc/plugin.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <expected>
#include <initializer_list>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

struct TestHandler {};

// Game.A and Game.B share one filename; the DLL profiles are observed independently.
struct TestCatalog {
    std::array<std::string_view, 1> game_names{"game.exe"};
    std::array<std::string_view, 1> peer_names{"peer.dll"};
    std::array<std::string_view, 1> later_names{"later.dll"};
    std::array<fc::runtime::SectionProfile, 1> sections{fc::runtime::SectionProfile{".text", 0x1000, 0x100}};
    std::array<fc::runtime::ImageProfile, 4> profiles{
        fc::runtime::ImageProfile{"Game.A", game_names, 0x2000, sections},
        fc::runtime::ImageProfile{"Game.B", game_names, 0x2000, sections},
        fc::runtime::ImageProfile{"Peer.Ready", peer_names, 0x2000, sections},
        fc::runtime::ImageProfile{"Later.Ready", later_names, 0x2000, sections},
    };
};

// Copies dependency names into storage owned by the SDK declaration.
std::vector<std::string> strings(std::initializer_list<std::string_view> values) {
    std::vector<std::string> result;

    for (const auto value : values) {
        result.emplace_back(value);
    }

    return result;
}

// Declares a handler variant with patch and variant dependencies and startup controls.
fc::Patch make_patch(std::string id, std::string_view selector = "Peer.Ready", std::initializer_list<std::string_view> dependencies = {},
                     std::initializer_list<std::string_view> variant_dependencies = {}, bool enabled = true, bool configurable = true) {
    auto variant = fc::variant<TestHandler>({selector});
    variant.depends_on(variant_dependencies);
    return fc::Patch{.id = std::move(id),
                     .enabled = enabled,
                     .configurable = configurable,
                     .depends_on = strings(dependencies),
                     .variants = {std::move(variant)}};
}

// Declares alternative game variants so late selection can add the chosen variant's dependencies to common ones.
fc::Patch make_choice_patch(std::string id, std::initializer_list<std::string_view> common_dependencies,
                            std::initializer_list<std::string_view> first_dependencies,
                            std::initializer_list<std::string_view> second_dependencies) {
    auto first = fc::variant<TestHandler>({"Game.A"});
    first.depends_on(first_dependencies);

    auto second = fc::variant<TestHandler>({"Game.B"});
    second.depends_on(second_dependencies);

    return fc::Patch{.id = std::move(id),
                     .enabled = true,
                     .depends_on = strings(common_dependencies),
                     .variants = {std::move(first), std::move(second)}};
}

// Packages patches and groups into a versioned plugin declaration for admission.
fc::Plugin make_plugin(std::string id, std::vector<fc::Patch> patches, std::vector<fc::Group> groups = {}) {
    return {.id = std::move(id), .version = "1", .groups = std::move(groups), .patches = std::move(patches)};
}

// Owns SDK declarations so their C records stay valid throughout each test.
class AdmittedWorld {
  public:
    // Lowers and admits SDK declarations, retaining their backing storage in this world.
    explicit AdmittedWorld(std::vector<fc::Plugin> plugins) {
        storage_.reserve(plugins.size());
        std::vector<fc::runtime::DefinitionCandidate> candidates;
        candidates.reserve(plugins.size());

        // Admission and sessions borrow the C records; keep each declaration at a fixed heap address.
        for (auto &plugin : plugins) {
            auto storage = std::make_unique<fc::detail::PluginStorage>(std::move(plugin));
            const auto id = fc::detail::view(storage->descriptor()->id);
            candidates.push_back({std::string{id}, storage->descriptor()});
            storage_.push_back(std::move(storage));
        }

        admission_ = fc::runtime::admit_definitions(candidates);
    }

    // Finds an admitted plugin by ASCII case-insensitive ID; fails the test on admission or lookup errors.
    const fc::runtime::AdmittedPlugin &plugin(std::string_view id) const {
        REQUIRE(admission_.diagnostics.empty());

        const auto found = std::ranges::find_if(admission_.admitted, [&](const fc::runtime::AdmittedPlugin &plugin) {
            return fc::detail::equal_id(fc::detail::view(plugin.definition->id), id);
        });

        REQUIRE(found != admission_.admitted.end());
        return *found;
    }

    // Resolves and configures client patches without configuration files; this world and catalog must outlive the session.
    fc::runtime::SelectionSession session(const TestCatalog &catalog, std::span<const fc::runtime::ImageObservation> images) const {
        auto result = fc::runtime::resolve_patches(admission_, catalog.profiles, FC_ROLE_CLIENT);
        fc::runtime::configure_patches(result, images, {{}, false});
        return result;
    }

  private:
    std::vector<std::unique_ptr<fc::detail::PluginStorage>> storage_;
    fc::runtime::AdmissionResult admission_;
};

// Observes Peer.Ready as present, the optional game profile, and the late DLL as absent.
// Returned profile pointers borrow catalog; game must refer into that same catalog when supplied.
std::vector<fc::runtime::ImageObservation> observations(const TestCatalog &catalog, const fc::runtime::ImageProfile *game = nullptr) {
    // Use an empty optional for an absent module; a present module refers to its catalog profile.
    const auto optional_profile = [](const fc::runtime::ImageProfile *profile) {
        return profile == nullptr ? std::optional<const fc::runtime::ImageProfile *>{}
                                  : std::optional<const fc::runtime::ImageProfile *>{profile};
    };

    return {
        {std::string{catalog.game_names[0]}, optional_profile(game)},
        {"peer.dll", std::optional<const fc::runtime::ImageProfile *>{&catalog.profiles[2]}},
        {"later.dll", optional_profile(nullptr)},
    };
}

// Finds a patch by ASCII case-insensitive plugin and patch IDs, failing the test if absent; the result borrows session.
const fc::runtime::PatchRecord &patch(const fc::runtime::SelectionSession &session, std::string_view plugin, std::string_view id) {
    const auto found = std::ranges::find_if(session.patches, [&](const auto &value) {
        return fc::detail::equal_id(fc::detail::view(value->plugin->admitted.definition->id), plugin) &&
               fc::detail::equal_id(fc::detail::view(value->definition->id), id);
    });

    REQUIRE(found != session.patches.end());
    return **found;
}
} // namespace

TEST_CASE("Control resolution applies patch and group precedence", "[dependency]") {
    AdmittedWorld world{
        {make_plugin("Controls",
                     {make_patch("Own", "Peer.Ready", {}, {}, true, true), make_patch("Inherited", "Peer.Ready", {}, {}, true, false),
                      make_patch("Mandatory", "Peer.Ready", {}, {}, true, false)},
                     {fc::Group{.id = "Bundle", .members = {"Own", "Inherited"}, .enabled = false, .configurable = true},
                      fc::Group{.id = "Fixed", .members = {"Mandatory"}, .enabled = false, .configurable = false}})}};
    const auto &plugin = world.plugin("controls");
    const std::array overrides{fc::runtime::ControlOverride{"oWn", false}, fc::runtime::ControlOverride{"BUNDLE", true}};

    const auto controls = fc::runtime::resolve_controls(plugin, overrides);

    CHECK_FALSE(controls[0].enabled);
    CHECK(controls[1].enabled);
    CHECK(controls[2].enabled);

    // Without overrides, configurable patches use their own defaults; other members inherit a configurable group's default.
    const auto defaults = fc::runtime::resolve_controls(plugin, {});
    CHECK(defaults[0].enabled);
    CHECK_FALSE(defaults[1].enabled);
    CHECK(defaults[2].enabled);
}

TEST_CASE("Dependencies expand local, qualified and group references without duplicate providers", "[dependency]") {
    TestCatalog catalog;
    AdmittedWorld world{
        {make_plugin("Beta", {make_patch("R"), make_patch("S")}, {fc::Group{.id = "Pair", .members = {"R", "S"}, .enabled = false}}),
         make_plugin("Alpha", {make_patch("P"), make_patch("Consumer", "Peer.Ready", {"p", "BETA.Pair"}, {"beta.r"})})}};

    const auto session = world.session(catalog, observations(catalog));
    const auto &consumer = patch(session, "Alpha", "Consumer");

    CHECK(consumer.state == fc::runtime::PatchState::Planning);
    REQUIRE(consumer.providers.size() == 3);
    CHECK(std::ranges::contains(consumer.providers, &patch(session, "Alpha", "P")));
    CHECK(std::ranges::contains(consumer.providers, &patch(session, "Beta", "R")));
    CHECK(std::ranges::contains(consumer.providers, &patch(session, "Beta", "S")));
}

TEST_CASE("Dependencies wait for images and never enable disabled providers", "[dependency]") {
    TestCatalog catalog;
    AdmittedWorld world{{make_plugin("States", {make_patch("Disabled", "Peer.Ready", {}, {}, false), make_patch("Pending", "Later.Ready"),
                                                make_patch("NeedsDisabled", "Peer.Ready", {"Disabled"}),
                                                make_patch("NeedsPending", "Peer.Ready", {"Pending"}),
                                                make_patch("Missing", "Peer.Ready", {"Pending", "Absent"})})}};

    const auto session = world.session(catalog, observations(catalog));
    CHECK(patch(session, "States", "Disabled").state == fc::runtime::PatchState::Disabled);
    CHECK_FALSE(patch(session, "States", "Disabled").enabled);
    CHECK(patch(session, "States", "Pending").state == fc::runtime::PatchState::Waiting);
    CHECK(patch(session, "States", "NeedsPending").state == fc::runtime::PatchState::Waiting);
    CHECK(patch(session, "States", "NeedsDisabled").state == fc::runtime::PatchState::Failed);
    CHECK(patch(session, "States", "Missing").state == fc::runtime::PatchState::Failed);
}

TEST_CASE("Dependency cycles fail participants and consumers while independent patches can plan", "[dependency]") {
    TestCatalog catalog;
    AdmittedWorld world{{make_plugin("Cycles", {make_patch("A", "Peer.Ready", {"B"}), make_patch("B", "Peer.Ready", {"A"}),
                                                make_patch("Consumer", "Peer.Ready", {"A"}), make_patch("Independent")})}};

    const auto session = world.session(catalog, observations(catalog));
    for (const auto id : {"A", "B", "Consumer"}) {
        CHECK(patch(session, "Cycles", id).state == fc::runtime::PatchState::Failed);
    }
    CHECK(patch(session, "Cycles", "Independent").state == fc::runtime::PatchState::Planning);
}

TEST_CASE("Late image selection adds only the chosen variant's dependencies", "[dependency]") {
    TestCatalog catalog;
    catalog.game_names[0] = "game.dll";
    AdmittedWorld world{{make_plugin("Late", {make_patch("Common"), make_patch("First"), make_patch("Second"),
                                              make_choice_patch("Consumer", {"Common"}, {"First"}, {"Second"})})}};

    auto session = world.session(catalog, observations(catalog));
    const auto &consumer = patch(session, "Late", "Consumer");

    CHECK(consumer.state == fc::runtime::PatchState::Waiting);
    REQUIRE(consumer.providers.size() == 1);
    CHECK(consumer.providers[0] == &patch(session, "Late", "Common"));

    // Loading Game.A adds its variant's dependency to the common dependency already recorded.
    fc::runtime::refresh_selection(session, observations(catalog, &catalog.profiles[0]));

    CHECK(consumer.state == fc::runtime::PatchState::Planning);
    REQUIRE(consumer.providers.size() == 2);
    CHECK(std::ranges::contains(consumer.providers, &patch(session, "Late", "First")));
    CHECK_FALSE(std::ranges::contains(consumer.providers, &patch(session, "Late", "Second")));
    CHECK(consumer.enabled);
}

TEST_CASE("Dependency failure uses patch policy while waiting and selected variant policy when ready", "[dependency]") {
    TestCatalog catalog;
    catalog.game_names[0] = "game.dll";
    for (const bool ready : {false, true}) {
        CAPTURE(ready);
        auto consumer = make_patch("Consumer", "Game.A", {"Absent"});
        consumer.failure_policy = fc::FailurePolicy::Fatal;
        consumer.variants[0].failure_policy(fc::FailurePolicy::Continue);
        AdmittedWorld world{{make_plugin("Policies", {std::move(consumer)})}};

        const auto session = world.session(catalog, observations(catalog, ready ? &catalog.profiles[0] : nullptr));
        CHECK(patch(session, "Policies", "Consumer").state == fc::runtime::PatchState::Failed);
        CHECK(session.fatal == !ready);
    }
}
