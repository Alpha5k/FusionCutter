#include "patches/selection.hpp"
#include "../../plugins/core.hpp"

#include <fc/plugin.hpp>

#include <catch2/catch_test_macros.hpp>

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

struct TestHandler {};

// Owns a valid catalog with filename constraints that exercise selector and overlap behavior.
// Keep this catalog alive at a fixed address while resolutions or observations refer to its profiles.
struct TestCatalog {
    std::array<std::string_view, 1> steam_names{"game.exe"};
    std::array<std::string_view, 1> gog_names{"game.exe"};
    std::array<std::string_view, 1> galaxy_names{"galaxy.dll"};
    std::array<std::string_view, 2> alias_a_names{"shared.dll", "a.dll"};
    std::array<std::string_view, 2> alias_b_names{"shared.dll", "b.dll"};
    std::array<fc::runtime::SectionProfile, 1> sections{fc::runtime::SectionProfile{".text", 0x1000, 0x100}};
    std::array<fc::runtime::ImageProfile, 5> profiles{
        fc::runtime::ImageProfile{"Steam.Game", steam_names, 0x2000, sections},
        fc::runtime::ImageProfile{"GOG.Game", gog_names, 0x2000, sections},
        fc::runtime::ImageProfile{"Galaxy.Peer", galaxy_names, 0x2000, sections},
        fc::runtime::ImageProfile{"Alias.A", alias_a_names, 0x2000, sections},
        fc::runtime::ImageProfile{"Alias.B", alias_b_names, 0x2000, sections},
    };
};

// Creates an enabled patch with the supplied ID and variants.
fc::Patch make_patch(std::string id, std::vector<fc::Variant> variants) {
    return fc::Patch{.id = std::move(id), .enabled = true, .variants = std::move(variants)};
}

// Wraps patch declarations in the selection test plugin.
fc::Plugin make_plugin(std::vector<fc::Patch> patches) {
    return {.id = "selection_test", .version = "1", .patches = std::move(patches)};
}

// Keeps SDK declaration storage alive while selection retains pointers to its C records.
class AdmittedFixture {
  public:
    // Retains the supplied SDK declaration and validates its C records for selection tests.
    explicit AdmittedFixture(fc::Plugin plugin) : storage_(std::move(plugin)) {
        admission_ = fc::runtime::admit_definitions({{"selection test", storage_.descriptor()}});
    }

    // Returns the sole admitted plugin, failing the test if validation rejected the declaration.
    const fc::runtime::AdmittedPlugin &plugin() const {
        REQUIRE(admission_.diagnostics.empty());
        REQUIRE(admission_.admitted.size() == 1);
        return admission_.admitted.front();
    }

  private:
    fc::detail::PluginStorage storage_;
    fc::runtime::AdmissionResult admission_;
};

// Owns an admitted declaration for as long as its resolved patch retains declaration pointers.
class ResolvedFixture {
  public:
    // Resolves one test patch, throwing its diagnostic on failure.
    // Keep catalog data alive and unchanged while this fixture or its returned patch is used.
    ResolvedFixture(std::vector<fc::Variant> variants, std::span<const fc::runtime::ImageProfile> catalog)
        : admitted_(make_plugin({make_patch("patch", std::move(variants))})) {
        auto result = fc::runtime::resolve_patch(admitted_.plugin(), 0, catalog);
        if (!result) {
            throw std::runtime_error{result.error().message};
        }

        patch_ = std::move(*result);
    }

    const fc::runtime::ResolvedPatch &patch() const {
        return patch_;
    }

  private:
    AdmittedFixture admitted_;
    fc::runtime::ResolvedPatch patch_;
};

// Supplies an explicit absence for every basename the resolved patch may inspect.
std::vector<fc::runtime::ImageObservation> absent_observations(const fc::runtime::ResolvedPatch &patch) {
    std::vector<fc::runtime::ImageObservation> result;
    std::vector<std::string> seen;

    // Preserve the first spelling encountered; aliases differing only by ASCII case share one observation.
    for (const auto &variant : patch.variants) {
        for (const auto &requirement : variant.images) {
            for (const auto *profile : requirement.profiles) {
                for (const auto filename : profile->filenames) {
                    const auto folded = fc::detail::folded_id(filename);
                    if (!std::ranges::contains(seen, folded)) {
                        seen.push_back(folded);
                        result.push_back({std::string{filename}, std::optional<const fc::runtime::ImageProfile *>{}});
                    }
                }
            }
        }
    }

    return result;
}

// Finds an existing observation by ASCII case-insensitive basename, failing the test when none exists.
fc::runtime::ImageObservation &observation(std::vector<fc::runtime::ImageObservation> &observations, std::string_view filename) {
    const auto found = std::ranges::find_if(observations, [&](const fc::runtime::ImageObservation &value) {
        return fc::detail::equal_id(value.filename, filename);
    });

    REQUIRE(found != observations.end());
    return *found;
}

// Replaces an existing basename's outcome with the supplied recognized profile.
// The profile must use the resolved patch's catalog and remain alive through evaluation.
void observe_profile(std::vector<fc::runtime::ImageObservation> &observations, std::string_view filename,
                     const fc::runtime::ImageProfile &profile) {
    observation(observations, filename).outcome = std::optional<const fc::runtime::ImageProfile *>{&profile};
}
} // namespace

TEST_CASE("Overlap rejection checks profiles, catalog filename hints, and launch roles") {
    TestCatalog catalog;

    SECTION("alternatives overlap a narrower variant") {
        AdmittedFixture admitted{make_plugin(
            {make_patch("conflict", {fc::variant<TestHandler>({"Steam.Game", "GOG.Game"}), fc::variant<TestHandler>({"Steam.Game"})})})};

        const auto result = fc::runtime::resolve_patch(admitted.plugin(), 0, catalog.profiles);

        REQUIRE_FALSE(result);
    }

    SECTION("renamed labels and a late required module still overlap") {
        AdmittedFixture admitted{
            make_plugin({make_patch("conflict", {fc::variant<TestHandler>({fc::ImageRequirement{"exe", {"Steam.Game"}}}),
                                                 fc::variant<TestHandler>({fc::ImageRequirement{"game", {"Steam.Game"}},
                                                                           fc::ImageRequirement{"peer", {"Galaxy.Peer"}}})})})};

        CHECK_FALSE(fc::runtime::resolve_patch(admitted.plugin(), 0, catalog.profiles));
    }

    SECTION("Client and Server occupy separate launch modes") {
        ResolvedFixture fixture{
            {fc::variant<TestHandler>({"Steam.Game"}, fc::Role::Client), fc::variant<TestHandler>({"Steam.Game"}, fc::Role::Server)},
            catalog.profiles};

        CHECK(fixture.patch().variants.size() == 2);
    }

    SECTION("distinct profiles sharing one basename remain separate variants") {
        ResolvedFixture fixture{{fc::variant<TestHandler>({"Steam.Game"}), fc::variant<TestHandler>({"GOG.Game"})}, catalog.profiles};

        CHECK(fixture.patch().variants.size() == 2);
    }

    SECTION("Variants overlap when aliased profiles can coexist under different filenames") {
        AdmittedFixture admitted{
            make_plugin({make_patch("conflict", {fc::variant<TestHandler>({"Alias.A"}), fc::variant<TestHandler>({"Alias.B"})})})};

        CHECK_FALSE(fc::runtime::resolve_patch(admitted.plugin(), 0, catalog.profiles));
    }
}

TEST_CASE("Selection distinguishes waiting, unavailable and inspection failure") {
    TestCatalog catalog;
    ResolvedFixture known{{fc::variant<TestHandler>({"Steam.Game"})}, catalog.profiles};
    auto observations = absent_observations(known.patch());

    auto selected = fc::runtime::evaluate_patch(known.patch(), FC_ROLE_CLIENT, observations);

    CHECK(selected.state == fc::runtime::SelectionState::Unavailable);
    CHECK(selected.not_applicable);

    observe_profile(observations, "game.exe", catalog.profiles[1]);
    observation(observations, "game.exe").executable = true;
    selected = fc::runtime::evaluate_patch(known.patch(), FC_ROLE_CLIENT, observations);

    CHECK(selected.state == fc::runtime::SelectionState::Unavailable);
    CHECK(selected.not_applicable);

    observation(observations, "game.exe").outcome = std::unexpected{fc::runtime::Diagnostic{"game.exe", "inspection failed", 123}};
    selected = fc::runtime::evaluate_patch(known.patch(), FC_ROLE_CLIENT, observations);

    REQUIRE(selected.state == fc::runtime::SelectionState::Unavailable);
    REQUIRE(selected.reasons.size() == 1);
    CHECK(selected.reasons[0].windows_error == 123);
    CHECK_FALSE(selected.not_applicable);

    observations = {{"renamed-tools.bin", std::unexpected{fc::runtime::Diagnostic{"renamed-tools.bin", "bad PE", 456}}, true}};
    selected = fc::runtime::evaluate_patch(known.patch(), FC_ROLE_CLIENT, observations);
    REQUIRE(selected.state == fc::runtime::SelectionState::Unavailable);
    REQUIRE(selected.reasons.size() == 1);
    CHECK(selected.reasons[0].windows_error == 456);
    CHECK(selected.reasons[0].message.find("bad PE") != std::string::npos);
    CHECK_FALSE(selected.not_applicable);

    ResolvedFixture unknown{{fc::variant<TestHandler>({"Missing.*"})}, catalog.profiles};
    selected = fc::runtime::evaluate_patch(unknown.patch(), FC_ROLE_CLIENT, {});

    CHECK(selected.state == fc::runtime::SelectionState::Unavailable);
    CHECK(selected.not_applicable);
}

TEST_CASE("Selection handles aliases, ambiguity, and alternatives by physical observation") {
    TestCatalog catalog;

    SECTION("a renamed executable binds by its recognized profile") {
        ResolvedFixture fixture{{fc::variant<TestHandler>({"Steam.Game"})}, catalog.profiles};
        auto observations = absent_observations(fixture.patch());
        observations.push_back({"renamed-tools.exe", &catalog.profiles[0], true});

        const auto selected = fc::runtime::evaluate_patch(fixture.patch(), FC_ROLE_CLIENT, observations);

        REQUIRE(selected.state == fc::runtime::SelectionState::Ready);
        REQUIRE(selected.images.size() == 1);
        CHECK(selected.images[0].observation_index == observations.size() - 1);
    }

    SECTION("renamed images that make two variants ready remain ambiguous") {
        ResolvedFixture fixture{{fc::variant<TestHandler>({"Steam.Game"}), fc::variant<TestHandler>({"GOG.Game"})}, catalog.profiles};
        auto observations = absent_observations(fixture.patch());
        observations.push_back({"renamed-steam.exe", &catalog.profiles[0]});
        observations.push_back({"renamed-gog.exe", &catalog.profiles[1]});

        const auto selected = fc::runtime::evaluate_patch(fixture.patch(), FC_ROLE_CLIENT, observations);

        CHECK(selected.state == fc::runtime::SelectionState::Unavailable);
        CHECK_FALSE(selected.not_applicable);
    }

    SECTION("a present executable alias wins over absent executable aliases") {
        catalog.alias_a_names = {"game.EXE", "game.original.exe"};
        ResolvedFixture fixture{{fc::variant<TestHandler>({"Alias.A"})}, catalog.profiles};
        auto observations = absent_observations(fixture.patch());

        const auto absent = fc::runtime::evaluate_patch(fixture.patch(), FC_ROLE_CLIENT, observations);
        CHECK(absent.state == fc::runtime::SelectionState::Unavailable);
        CHECK(absent.not_applicable);

        observe_profile(observations, "game.original.exe", catalog.profiles[3]);
        const auto selected = fc::runtime::evaluate_patch(fixture.patch(), FC_ROLE_CLIENT, observations);

        REQUIRE(selected.state == fc::runtime::SelectionState::Ready);
        CHECK(selected.images[0].profile == &catalog.profiles[3]);
    }

    SECTION("another declared profile can match the present alias") {
        ResolvedFixture fixture{{fc::variant<TestHandler>({"Alias.A", "Alias.B"})}, catalog.profiles};
        auto observations = absent_observations(fixture.patch());
        observe_profile(observations, "shared.dll", catalog.profiles[4]);

        const auto selected = fc::runtime::evaluate_patch(fixture.patch(), FC_ROLE_CLIENT, observations);

        REQUIRE(selected.state == fc::runtime::SelectionState::Ready);
        CHECK(selected.images[0].profile == &catalog.profiles[4]);
    }

    SECTION("two physical matches for one label are ambiguous") {
        ResolvedFixture fixture{{fc::variant<TestHandler>({"Alias.A"})}, catalog.profiles};
        auto observations = absent_observations(fixture.patch());
        observe_profile(observations, "shared.dll", catalog.profiles[3]);
        observe_profile(observations, "a.dll", catalog.profiles[3]);

        const auto selected = fc::runtime::evaluate_patch(fixture.patch(), FC_ROLE_CLIENT, observations);

        CHECK(selected.state == fc::runtime::SelectionState::Unavailable);
    }

    SECTION("aliases of the same live mapping bind once") {
        ResolvedFixture fixture{{fc::variant<TestHandler>({"Alias.A"})}, catalog.profiles};
        fc::runtime::LiveImageObservations images{
            {"shared.dll", fc::runtime::LoadedImage{&catalog.profiles[3], "shared.dll", 0x100000, 0x2000}},
            {"a.dll", fc::runtime::LoadedImage{&catalog.profiles[3], "shared.dll", 0x100000, 0x2000}}};

        auto selected = fc::runtime::evaluate_patch(fixture.patch(), FC_ROLE_CLIENT, fc::runtime::image_observations(images));

        REQUIRE(selected.state == fc::runtime::SelectionState::Ready);
        REQUIRE(selected.images.size() == 1);
        CHECK(selected.images[0].observation_index == 0);

        (*images[1].outcome)->base = 0x200000;
        selected = fc::runtime::evaluate_patch(fixture.patch(), FC_ROLE_CLIENT, fc::runtime::image_observations(images));
        CHECK(selected.state == fc::runtime::SelectionState::Unavailable);
    }
}

TEST_CASE("Classic game aliases bind a recognized original beside a present incompatible proxy") {
    // Use different fixture DLLs for the proxy and the original so only the original matches the profile.
    const auto directory = std::filesystem::path{FC_FIXTURE_PLUGIN}.parent_path() / "classic-aliases";
    std::filesystem::create_directories(directory);
    const auto proxy_path = directory / "Battlefront2.dll", original_path = directory / "Battlefront2.original.dll";
    std::filesystem::copy_file(FC_FIXTURE_PLUGIN, proxy_path, std::filesystem::copy_options::overwrite_existing);
    std::filesystem::copy_file(FC_FIXTURE_NO_EXPORT, original_path, std::filesystem::copy_options::overwrite_existing);

    std::ifstream input{original_path, std::ios::binary | std::ios::ate};
    std::vector<std::byte> bytes(static_cast<std::size_t>(input.tellg()));
    input.seekg(0);
    REQUIRE(input.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size())));
    const auto layout = fc::runtime::parse_file_image(bytes, "Classic original fixture");
    REQUIRE(layout);

    // Describe the original DLL's PE sections while accepting both installed filenames.
    std::vector<fc::runtime::SectionProfile> sections;
    for (const auto &section : layout->sections) {
        sections.push_back({section.name, section.rva, section.virtual_size});
    }

    const std::array<std::string_view, 2> names{"Battlefront2.dll", "Battlefront2.original.dll"};
    const std::array catalog{fc::runtime::ImageProfile{"Classic.Game", names, layout->image_size, sections}};
    AdmittedFixture fixture{make_plugin({make_patch("classic", {fc::variant<TestHandler>({"Classic.Game"})})})};

    const auto resolved = fc::runtime::resolve_patch(fixture.plugin(), 0, catalog);
    REQUIRE(resolved);

    const auto proxy = LoadLibraryW(proxy_path.c_str()), original = LoadLibraryW(original_path.c_str());
    REQUIRE(proxy);
    REQUIRE(original);

    const std::array patches{&*resolved};
    auto observed = fc::runtime::observe_loaded_images(patches, catalog);
    const auto selected = fc::runtime::evaluate_patch(*resolved, FC_ROLE_CLIENT, fc::runtime::image_observations(observed));

    CHECK(selected.state == fc::runtime::SelectionState::Ready);
    REQUIRE(selected.images.size() == 1);
    CHECK(observed[selected.images[0].observation_index].filename == "Battlefront2.original.dll");

    const auto index = selected.images[0].observation_index;
    REQUIRE(observed[index].outcome);
    REQUIRE(*observed[index].outcome);
    CHECK((*observed[index].outcome)->base == reinterpret_cast<std::uintptr_t>(original));
    CHECK_FALSE(observed[0].outcome);

    // Appending another absent basename leaves the selected original's binding and pinned mapping unchanged.
    observed.push_back({"later.dll", std::nullopt});
    const auto refreshed = fc::runtime::evaluate_patch(*resolved, FC_ROLE_CLIENT, fc::runtime::image_observations(observed));
    REQUIRE(refreshed.state == fc::runtime::SelectionState::Ready);
    CHECK(refreshed.images[0].observation_index == index);

    FreeLibrary(proxy);
    FreeLibrary(original);
}

TEST_CASE("Selection waits for every required image before returning bindings") {
    TestCatalog catalog;
    ResolvedFixture fixture{
        {fc::variant<TestHandler>({fc::ImageRequirement{"game", {"sTeAm*", "Steam.Game"}}, fc::ImageRequirement{"peer", {"gAl*"}}})},
        catalog.profiles};
    auto observations = absent_observations(fixture.patch());
    observe_profile(observations, "game.exe", catalog.profiles[0]);

    auto selected = fc::runtime::evaluate_patch(fixture.patch(), FC_ROLE_CLIENT, observations);
    CHECK(selected.state == fc::runtime::SelectionState::Waiting);
    CHECK(selected.images.empty());

    observe_profile(observations, "galaxy.dll", catalog.profiles[2]);
    selected = fc::runtime::evaluate_patch(fixture.patch(), FC_ROLE_CLIENT, observations);
    REQUIRE(selected.state == fc::runtime::SelectionState::Ready);
    REQUIRE(selected.images.size() == 2);
    CHECK(selected.images[0].profile == &catalog.profiles[0]);
    CHECK(selected.images[1].profile == &catalog.profiles[2]);
}

TEST_CASE("Core selects its client, Lua and server patches for their supported builds and launch roles") {
    const auto *definition = fc::core_plugin::core_plugin();
    const auto admission = fc::runtime::admit_definitions({{"Core", definition}});
    REQUIRE(admission.diagnostics.empty());
    REQUIRE(admission.admitted.size() == 1);
    const auto catalog = fc::runtime::known_image_profiles();
    const std::array<std::string_view, 2> rcon_profiles{"BF2.GOG.59EDF52B", "BF2.Classic.66702CD2"};

    for (std::size_t patch_index = 0; patch_index < definition->patch_count; ++patch_index) {
        const auto &patch = definition->patches[patch_index];
        const auto id = fc::detail::view(patch.id);
        CAPTURE(id);
        const auto resolved = fc::runtime::resolve_patch(admission.admitted[0], patch_index, catalog);
        REQUIRE(resolved);

        // Empty expansions are expected when a valid Core declaration targets another architecture's game builds.
        if (std::ranges::all_of(resolved->variants, [](const auto &variant) {
                return std::ranges::any_of(variant.images, [](const auto &image) {
                    return image.profiles.empty();
                });
            })) {
            const auto selected = fc::runtime::evaluate_patch(*resolved, FC_ROLE_CLIENT, {});
            CHECK(selected.not_applicable);
        }

        // Model one present build at a time, leaving the other candidate basenames absent.
        for (const auto &profile : catalog) {
            auto observations = absent_observations(*resolved);
            for (auto &value : observations) {
                if (std::ranges::contains(profile.filenames, std::string_view{value.filename})) {
                    value.outcome = std::optional<const fc::runtime::ImageProfile *>{&profile};
#if !defined(_WIN64)
                    value.executable = true;
#endif
                    break;
                }
            }

            const bool lua_patch = id == "lua_bindings" || id == "lua_multiplayer";
            const bool client_patch = id == "ColoredChats" || id == "UnlockFrameRate";
            const bool supported_build =
                id == "RconServer" ? std::ranges::contains(rcon_profiles, profile.id)
                                   : profile.id == "BF2.Steam.59EDE353" || profile.id == "BF2.GOG.59EDF52B" ||
                                         ((id == "lua_bindings" || id == "UnlockFrameRate") && profile.id == "BF2.ModTools.43EBD102");
            for (const auto role : {FC_ROLE_CLIENT, FC_ROLE_SERVER}) {
                CAPTURE(profile.id, role);
                const bool supported_role = lua_patch || (client_patch ? role == FC_ROLE_CLIENT : role == FC_ROLE_SERVER);
                const auto selected = fc::runtime::evaluate_patch(*resolved, role, observations);
                if (supported_build && supported_role) {
                    REQUIRE(selected.state == fc::runtime::SelectionState::Ready);
                    REQUIRE(selected.images.size() == 1);
                    CHECK(selected.images[0].profile == &profile);
                    if (id == "RconServer") {
                        CHECK(rcon_profiles[selected.variant_index] == profile.id);
                    }
                } else {
                    CHECK(selected.state != fc::runtime::SelectionState::Ready);
                    CHECK(selected.variant == nullptr);
                }
                if (!supported_role) {
                    CHECK(selected.state == fc::runtime::SelectionState::Unavailable);
                    CHECK(selected.not_applicable);
                }
#if !defined(_WIN64)
                if (profile.id.starts_with("BF1.")) {
                    CHECK(selected.state == fc::runtime::SelectionState::Unavailable);
                    CHECK(selected.not_applicable);
                }
#endif
            }
        }
    }
}

TEST_CASE("Selection filters concrete launch roles and reuses resolutions with later observations") {
    TestCatalog catalog;
    ResolvedFixture fixture{
        {fc::variant<TestHandler>({"sTeAm*"}, fc::Role::Client), fc::variant<TestHandler>({"Galaxy.Peer"}, fc::Role::Server)},
        catalog.profiles};
    auto observations = absent_observations(fixture.patch());

    auto selected = fc::runtime::evaluate_patch(fixture.patch(), FC_ROLE_CLIENT, observations);

    CHECK(selected.state == fc::runtime::SelectionState::Unavailable);
    CHECK(selected.not_applicable);

    observe_profile(observations, "game.exe", catalog.profiles[0]);
    selected = fc::runtime::evaluate_patch(fixture.patch(), FC_ROLE_CLIENT, observations);

    REQUIRE(selected.state == fc::runtime::SelectionState::Ready);
    CHECK(selected.variant_index == 0);

    selected = fc::runtime::evaluate_patch(fixture.patch(), FC_ROLE_SERVER, observations);

    CHECK(selected.state == fc::runtime::SelectionState::Waiting);

    ResolvedFixture all_roles{{fc::variant<TestHandler>({"Steam.Game"}, fc::Role::All)}, catalog.profiles};
    auto all_observations = absent_observations(all_roles.patch());
    observe_profile(all_observations, "game.exe", catalog.profiles[0]);

    for (const auto role : {FC_ROLE_CLIENT, FC_ROLE_SERVER}) {
        selected = fc::runtime::evaluate_patch(all_roles.patch(), role, all_observations);

        CHECK(selected.state == fc::runtime::SelectionState::Ready);
    }
}
