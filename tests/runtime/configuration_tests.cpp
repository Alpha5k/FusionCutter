#include "patches/patch_record.hpp"
#include "../test_support.hpp"
#include "../../plugins/core.hpp"

#include <fc/plugin.hpp>
#include <catch2/catch_test_macros.hpp>
#include <Windows.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <utility>
#include <cstdint>
#include <filesystem>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

// Supplies two executable builds and an optional peer DLL for ready and waiting configuration scenarios.
struct Catalog {
    std::array<std::string_view, 1> game{"game.exe"}, peer{"peer.dll"};
    std::array<fc::runtime::SectionProfile, 1> sections{{{".text", 0x1000, 0x100}}};
    std::array<fc::runtime::ImageProfile, 3> profiles{
        {{"Game.A", game, 0x2000, sections}, {"Game.B", game, 0x3000, sections}, {"Peer.A", peer, 0x2000, sections}}};

    // Game.A is present; the peer observation can become ready after the session captures settings.
    // Returned profile pointers borrow this catalog.
    std::vector<fc::runtime::ImageObservation> observations(bool present_peer = false) {
        return {{"game.exe", &profiles[0]},
                {"peer.dll", present_peer ? std::optional<const fc::runtime::ImageProfile *>{&profiles[2]} : std::nullopt}};
    }
};

// Owns each test's INI files under a Unicode path and removes them through the shared temporary-directory fixture.
struct Directory : fc::tests::TemporaryDirectory {
    // Renames the unique directory to exercise native filesystem paths and UTF-8 diagnostic labels together.
    Directory() {
        auto unicode = path;
        unicode += L"-\u914d\u7f6e";
        REQUIRE(MoveFileW(path.c_str(), unicode.c_str()));
        path = std::move(unicode);
    }

    // Overwrites a fixture file with the supplied bytes.
    void write(std::string_view name, std::string_view text) {
        std::ofstream output{path / name, std::ios::binary};
        output << text;
    }

    // Reads a fixture file as bytes, preserving its line endings.
    std::string read(std::string_view name) {
        return fc::tests::read_text(path / name);
    }
};

// Supplies typed scalar defaults and a value derived after settings validation.
struct Options {
    static inline int initializations{};

    // Counts default-object construction separately from copies used to create handlers.
    Options() {
        ++initializations;
    }

    std::uint8_t count = 10;
    bool flag = false;
    std::string color = "abcdef";
    int derived{};
};

// Rejects individual text input with a Windows error code that must survive runtime warning capture.
fc::Result<> color(std::string_view value) {
    return value.size() == 6 ? fc::Result<>{} : std::unexpected(fc::Error{"Color needs six characters", ERROR_INVALID_DATA});
}

// Derives a value after accepted scalars have been assigned to a copy of the defaults.
fc::Result<> finish(Options &value) {
    value.derived = value.count * 2;
    return {};
}

// Retains constructed settings so assertions observe the values passed through the real handler factory.
struct Handler {
    using Settings = Options;
    // Points into the most recently constructed handler while its session keeps the handler alive.
    static inline Options *observed{};
    Options options;

    // Copies completed settings and exposes this handler's copy to assertions.
    Handler(const fc::CreateContext &, const Options &value) : options(value) {
        observed = &options;
    }
};

// Supplies a patch with no settings or lifecycle callbacks.
struct EmptyHandler {};

// Builds the common field declarations, including precedence, constraints and completion validation.
auto schema() {
    auto result = fc::settings<Options>({fc::setting("count", &Options::count, "Counter limit.").range(1, 200).environment("FC_TEST_COUNT"),
                                         fc::setting("flag", &Options::flag, "Flag."),
                                         fc::setting("color", &Options::color, "Color.").validate(&color).section("display")});
    result.validate = &finish;
    return result;
}

// Selects optional, mandatory or peer-waiting declarations without changing the settings under test.
fc::Plugin plugin(fc::SettingsSchema settings, bool mandatory = false, bool waiting = false) {
    return {.id = "Example",
            .version = "1",
            .patches = {fc::Patch{.id = "patch",
                                  .enabled = true,
                                  .configurable = !mandatory,
                                  .failure_policy = mandatory ? fc::FailurePolicy::Fatal : fc::FailurePolicy::Continue,
                                  .settings = std::move(settings),
                                  .variants = {waiting ? fc::variant<Handler>({{"game", {"Game.A"}}, {"peer", {"Peer.A"}}})
                                                       : fc::variant<Handler>({"Game.A"})}}}};
}

// Uses production admission, resolution and configuration capture; storage and catalog outlive the returned session.
fc::runtime::SelectionSession capture(fc::detail::PluginStorage &storage, Catalog &catalog, Directory &directory, bool peer = false) {
    auto admitted = fc::runtime::admit_definitions({{"embedded", storage.descriptor()}});
    REQUIRE(admitted.admitted.size() == 1);
    auto session = fc::runtime::resolve_patches(std::move(admitted), catalog.profiles, FC_ROLE_CLIENT);
    fc::runtime::configure_patches(session, catalog.observations(peer), {directory.path});
    return session;
}
} // namespace

TEST_CASE("INI merges repeated sections and rejects malformed structure and duplicate keys") {
    // The JSON surrogate pair decodes to UTF-8; unquoted # and ; remain part of the value.
    auto ini = fc::runtime::parse_ini("; comment\n[a]\nfirst = \"literal #; \\uD83D\\uDE00\"\n[A]\nsecond = unquoted #;\n", "test.ini");

    REQUIRE(ini);
    REQUIRE(ini->entries.size() == 2);
    CHECK(ini->entries[0].value == "literal #; \xf0\x9f\x98\x80");
    CHECK(ini->entries[1].value == "unquoted #;");

    // Case-insensitive section/key identity makes x and X duplicates across [a] and [A].
    CHECK_FALSE(fc::runtime::parse_ini("[a]\nx=1\n[A]\nX=2\n", "test.ini"));
    CHECK_FALSE(fc::runtime::parse_ini("[broken\n", "test.ini"));
}

TEST_CASE("typed settings keep defaults for invalid fields and validate the completed object") {
    Directory directory;
    Catalog catalog;
    directory.write("FC.Example.ini", "[patch]\ncount=256\nflag=true\n[patch.display]\ncolor=bad\n");
    SetEnvironmentVariableW(L"FC_TEST_COUNT", nullptr);
    const auto before = Options::initializations;
    fc::detail::PluginStorage storage{plugin(schema())};
    auto session = capture(storage, catalog, directory);

    const auto &warnings = session.plugins[0]->configuration->warnings;
    REQUIRE(warnings.size() == 2);
    CHECK(std::ranges::any_of(warnings, [](const auto &warning) {
        return warning.windows_error == ERROR_INVALID_DATA;
    }));
    REQUIRE(fc::runtime::construct_handler(*session.patches[0], nullptr));
    REQUIRE(Handler::observed);
    CHECK(Handler::observed->count == 10);
    CHECK(Handler::observed->flag);
    CHECK(Handler::observed->color == "abcdef");
    CHECK(Handler::observed->derived == 20);
    CHECK(Options::initializations == before + 1);
}

TEST_CASE("a renamed executable contributes settings for its recognized build") {
    Directory directory;
    Catalog catalog;
    fc::detail::PluginStorage storage{plugin(schema())};
    auto admitted = fc::runtime::admit_definitions({{"embedded", storage.descriptor()}});
    auto session = fc::runtime::resolve_patches(std::move(admitted), catalog.profiles, FC_ROLE_CLIENT);
    auto observations = catalog.observations();
    observations[0].filename = "renamed-tools.exe";
    observations[0].executable = true;
    directory.write("FC.Example.ini", "[patch]\ncount=42\n");

    fc::runtime::configure_patches(session, observations, {directory.path});

    REQUIRE(session.patches[0]->state == fc::runtime::PatchState::Planning);
    REQUIRE(fc::runtime::construct_handler(*session.patches[0], nullptr));
    CHECK(Handler::observed->count == 42);
}

TEST_CASE("environment input overrides INI values and waiting patches retain captured settings") {
    Directory directory;
    Catalog catalog;
    directory.write("FC.Example.ini", "[patch]\ncount=42\n");
    SetEnvironmentVariableW(L"FC_TEST_COUNT", L"bad");
    fc::detail::PluginStorage storage{plugin(schema(), false, true)};

    auto session = capture(storage, catalog, directory);

    REQUIRE(session.patches[0]->state == fc::runtime::PatchState::Waiting);
    // Edit both sources after capture; the late image must retain the default chosen for invalid environment input.
    directory.write("FC.Example.ini", "[patch]\ncount=99\n");
    SetEnvironmentVariableW(L"FC_TEST_COUNT", L"55");

    fc::runtime::refresh_selection(session, catalog.observations(true));

    REQUIRE(session.patches[0]->state == fc::runtime::PatchState::Planning);
    REQUIRE(fc::runtime::construct_handler(*session.patches[0], nullptr));
    CHECK(Handler::observed->count == 10);

    auto fresh = capture(storage, catalog, directory, true);
    REQUIRE(fc::runtime::construct_handler(*fresh.patches[0], nullptr));
    CHECK(Handler::observed->count == 55);
    SetEnvironmentVariableW(L"FC_TEST_COUNT", nullptr);
}

TEST_CASE("completed validators remain distinct and reject invalid settings or defaults") {
    Directory directory;
    Catalog catalog;

    auto first = schema(), second = first;
    // Distinct derived values identify which completion callback each factory invokes.
    first.validate = +[](Options &options) -> fc::Result<> {
        options.derived = 11;
        return {};
    };
    second.validate = +[](Options &options) -> fc::Result<> {
        options.derived = 22;
        return {};
    };

    fc::detail::PluginStorage storage{fc::Plugin{
        .id = "Example",
        .version = "1",
        .patches = {fc::Patch{.id = "first", .enabled = true, .settings = first, .variants = {fc::variant<Handler>({"Game.A"})}},
                    fc::Patch{.id = "second", .enabled = true, .settings = second, .variants = {fc::variant<Handler>({"Game.A"})}}}}};

    auto session = capture(storage, catalog, directory);

    // Handler::observed changes on construction, so check each callback's result before creating the next handler.
    REQUIRE(fc::runtime::construct_handler(*session.patches[0], nullptr));
    CHECK(Handler::observed->derived == 11);

    REQUIRE(fc::runtime::construct_handler(*session.patches[1], nullptr));
    CHECK(Handler::observed->derived == 22);

    auto rejected = schema();
    // Rejects the completed object after scalar validation succeeds.
    rejected.validate = +[](Options &) -> fc::Result<> {
        return std::unexpected(fc::Error{"Fields conflict"});
    };
    fc::detail::PluginStorage rejected_storage{plugin(rejected)};

    auto rejected_session = capture(rejected_storage, catalog, directory);
    auto result = fc::runtime::construct_handler(*rejected_session.patches[0], nullptr);

    REQUIRE_FALSE(result);
    CHECK(result.error().message == "Fields conflict");
    CHECK(rejected_session.patches[0]->state == fc::runtime::PatchState::Failed);

    // Declaration defaults are validated during admission before the runtime can capture user configuration.
    auto bad = fc::settings<Options>({fc::setting("count", &Options::count, "Count.").range(20, 30)});
    fc::detail::PluginStorage invalid{plugin(bad)};

    const auto admission = fc::runtime::admit_definitions({{"invalid", invalid.descriptor()}});

    REQUIRE(admission.admitted.empty());
    CHECK_FALSE(admission.diagnostics.empty());
}

TEST_CASE("variant settings replace the patch schema and configuration preserves comments and values") {
    Directory directory;
    Catalog catalog;
    directory.write("FC.Example.ini",
                    "; saved\r\n[PaTcH]\r\ncount=33\r\n; Default: user comment\r\nunknown=keep\r\n[Other]\ncustom=unchanged");
    auto definition = plugin(schema());
    // The explicit variant schema replaces both patch fields and its completed-settings callback.
    definition.patches[0].variants[0].settings(fc::settings<Options>({fc::setting("flag", &Options::flag)}));
    fc::detail::PluginStorage storage{std::move(definition)};

    auto session = capture(storage, catalog, directory);
    REQUIRE(fc::runtime::construct_handler(*session.patches[0], nullptr));

    CHECK(Handler::observed->count == 10);
    CHECK(Handler::observed->derived == 0);

    const auto generated = directory.read("FC.Example.ini");
    CHECK(generated.starts_with("; saved\r\n[PaTcH]\r\ncount=33\r\n; Default: user comment\r\nunknown=keep\r\n"));
    CHECK(generated.ends_with("[Other]\ncustom=unchanged"));
    CHECK(generated.find("flag = false\n") != std::string::npos);
    const auto parsed = fc::runtime::parse_ini(generated, "generated.ini");
    REQUIRE(parsed);
    CHECK(fc::runtime::configuration_detail::find(*parsed, "patch", "flag"));
    CHECK(fc::runtime::configuration_detail::find(*parsed, "patch", "Enabled"));
    auto repeated = capture(storage, catalog, directory);
    CHECK(directory.read("FC.Example.ini") == generated);
    CHECK(generated.find("color") == std::string::npos);
}

TEST_CASE("configuration create and read failures reject loading while update failures retain values") {
    Directory directory;
    Catalog catalog;
    fc::detail::PluginStorage storage{plugin(schema())};
    auto admitted = fc::runtime::admit_definitions({{"embedded", storage.descriptor()}});
    auto missing = fc::runtime::resolve_patches(std::move(admitted), catalog.profiles, FC_ROLE_CLIENT);
    fc::runtime::configure_patches(missing, catalog.observations(), {directory.path / "missing"});
    CHECK(missing.plugins[0]->failure);
    CHECK_FALSE(fc::runtime::load_runtime_configuration({directory.path / "missing"}));

    directory.write("FC.Example.ini", "[patch]\ncount=40\n");
    REQUIRE(SetFileAttributesW((directory.path / "FC.Example.ini").c_str(), FILE_ATTRIBUTE_READONLY));
    auto existing = capture(storage, catalog, directory);
    REQUIRE(SetFileAttributesW((directory.path / "FC.Example.ini").c_str(), FILE_ATTRIBUTE_NORMAL));
    REQUIRE(existing.plugins[0]->configuration);
    CHECK_FALSE(existing.plugins[0]->configuration->warnings.empty());
    CHECK(directory.read("FC.Example.ini") == "[patch]\ncount=40\n");
    REQUIRE(fc::runtime::construct_handler(*existing.patches[0], nullptr));
    CHECK(Handler::observed->count == 40);

    // An exclusive file handle prevents both configuration loaders from reading their existing inputs.
    directory.write("FusionCutter.ini", "[FusionCutter]\nLogLevel=Info\n");
    for (const auto name : {L"FC.Example.ini", L"FusionCutter.ini"}) {
        const auto handle =
            CreateFileW((directory.path / name).c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        REQUIRE(handle != INVALID_HANDLE_VALUE);
        if (std::wstring_view{name} == L"FC.Example.ini") {
            auto unreadable = capture(storage, catalog, directory);
            CHECK(unreadable.plugins[0]->failure);
        } else {
            CHECK_FALSE(fc::runtime::load_runtime_configuration({directory.path}));
        }
        CloseHandle(handle);
    }
}

TEST_CASE("runtime settings default invalid values individually") {
    Directory directory;
    const auto defaults = fc::runtime::load_runtime_configuration({directory.path});
    REQUIRE(defaults);
    CHECK(defaults->log_level == FC_LOG_WARNING);
    CHECK(defaults->trace_file_bytes == 512ull * 1048576);

    // Zero is a valid trace budget and disables recording even when the log level is invalid.
    directory.write("FusionCutter.ini", "[FusionCutter]\nLogLevel=wrong\nMaxTraceSizeMB=0\n");
    const auto invalid_level = fc::runtime::load_runtime_configuration({directory.path});
    REQUIRE(invalid_level);
    CHECK(invalid_level->log_level == FC_LOG_WARNING);
    CHECK(invalid_level->trace_file_bytes == 0);
    CHECK(invalid_level->warnings.size() == 1);

    directory.write("FusionCutter.ini", "[FusionCutter]\nLogLevel=dEbUg\nMaxTraceSizeMB=18446744073709551615\n");
    const auto overflow = fc::runtime::load_runtime_configuration({directory.path});
    REQUIRE(overflow);
    CHECK(overflow->log_level == FC_LOG_DEBUG);
    CHECK(overflow->trace_file_bytes == 512ull * 1048576);
    CHECK(overflow->warnings.size() == 1);
}

TEST_CASE("generated category ordering preserves scalar declaration order and multiline comments") {
    Directory directory;
    Catalog catalog;

    auto definition = plugin(schema());
    definition.categories = {{.id = "Later", .order = 20}, {.id = "First", .order = 0}, {.id = "Other"}};
    definition.patches[0].description = "First line.\nSecond line.";
    definition.patches[0].category = "Later";
    for (const auto category : {"Other", "First"}) {
        auto patch = definition.patches[0];
        patch.id = std::string{category} + "Patch";
        patch.category = category;
        definition.patches.push_back(std::move(patch));
    }

    fc::detail::PluginStorage storage{std::move(definition)};

    auto session = capture(storage, catalog, directory);
    const auto output = directory.read("FC.Example.ini");

    CHECK(output.find("[First]") < output.find("[Later]"));
    CHECK(output.find("[Later]") < output.find("[Other]"));
    CHECK(output.find("count =") < output.find("flag ="));
    CHECK_FALSE(std::filesystem::exists(directory.path / "Example.ini"));
    // Each description line needs its own comment prefix to keep the generated INI parseable.
    CHECK(output.find("; First line.\n; Second line.\n") != std::string::npos);
    CHECK(fc::runtime::parse_ini(output, "generated.ini"));
    auto repeated = capture(storage, catalog, directory);
    CHECK(directory.read("FC.Example.ini") == output);
}

TEST_CASE("plugins without applicable configuration leave missing files absent and still reject malformed input") {
    Directory directory;
    Catalog catalog;
    auto definition = plugin({}, true);
    definition.patches[0].variants = {fc::variant<EmptyHandler>({"Game.A"})};
    fc::detail::PluginStorage storage{std::move(definition)};

    auto session = capture(storage, catalog, directory);
    REQUIRE(session.plugins[0]->configuration);
    CHECK_FALSE(std::filesystem::exists(directory.path / "FC.Example.ini"));

    directory.write("FC.Example.ini", "; user file\n");
    auto existing = capture(storage, catalog, directory);
    REQUIRE(existing.plugins[0]->configuration);
    CHECK(directory.read("FC.Example.ini") == "; user file\n");

    directory.write("FC.Example.ini", "[broken\n");
    auto malformed = capture(storage, catalog, directory);
    CHECK(malformed.plugins[0]->failure);
    CHECK(directory.read("FC.Example.ini") == "[broken\n");
}

TEST_CASE("mandatory configuration failure enters the production fatal boundary", "[.mandatory-child]") {
    // This hidden case runs in a child because enforcing its mandatory failure terminates the process.
    Directory directory;
    Catalog catalog;
    directory.write("FC.Example.ini", "[broken\n");
    auto definition = plugin(schema(), true);
    definition.patches[0].failure_policy = fc::FailurePolicy::Continue;
    definition.patches[0].variants[0].failure_policy(fc::FailurePolicy::Fatal);
    fc::detail::PluginStorage storage{std::move(definition)};
    auto session = capture(storage, catalog, directory);
    fc::runtime::enforce_startup_policy(session);
    FAIL("Required startup policy returned");
}

TEST_CASE("mandatory malformed configuration stops the child process") {
    // The hidden selector isolates the process-terminating case from the parent test run.
    std::array<wchar_t, 32768> path{};
    REQUIRE(GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size())));

    auto command = L"\"" + std::wstring{path.data()} + L"\" \"[.mandatory-child]\"";
    const auto code = fc::tests::run_child(std::move(command), nullptr, 10000, CREATE_NO_WINDOW);

    // 0xd1 identifies termination through the production fatal boundary.
    CHECK(code == 0xd1);
}

namespace {

// Uses legal INI keys whose spellings would be special names or path syntax in a filesystem.
struct Names {
    int device = 1;
    float fov = 90;
    fc::StringMap paths{{"path/color", "blue"}};
};

// Two builds share one scalar default while differing on another.
struct DefaultA {
    int limit = 10, shared = 7;
};

// Supplies a second numeric build default without changing the common value.
struct DefaultB {
    int limit = 20, shared = 7;
};

// Observes settings supplied through the selected variant's typed factory.
template <class T> struct SettingsHandler {
    using Settings = T;
    // Borrows the latest handler's settings while the owning session keeps that handler alive.
    static inline T *observed{};
    T options;

    // Copies the selected variant's completed settings and exposes the copy to assertions.
    SettingsHandler(const fc::CreateContext &, const T &value) : options(value) {
        observed = &options;
    }
};

// Shares key names across builds so generation must distinguish common and build-specific defaults.
template <class T> auto alternative_schema() {
    return fc::settings<T>({fc::setting("limit", &T::limit, "Build limit."), fc::setting("shared", &T::shared, "Shared default.")});
}

// Each image profile selects its own effective settings schema and handler type.
fc::Plugin alternative_plugin() {
    return {
        .id = "Example",
        .version = "1",
        .patches = {fc::Patch{.id = "patch",
                              .enabled = true,
                              .variants = {fc::variant<SettingsHandler<DefaultA>>({"Game.A"}).settings(alternative_schema<DefaultA>()),
                                           fc::variant<SettingsHandler<DefaultB>>({"Game.B"}).settings(alternative_schema<DefaultB>())}}}};
}

// A missing DLL leaves both variants waiting; an explicit profile selects a build at capture.
// Declaration storage and catalog profiles must outlive the returned session.
auto capture_alternatives(fc::detail::PluginStorage &storage, Catalog &catalog, Directory &directory,
                          const fc::runtime::ImageProfile *image = nullptr) {
    auto admitted = fc::runtime::admit_definitions({{"embedded", storage.descriptor()}});
    REQUIRE(admitted.admitted.size() == 1);

    const std::array observations{fc::runtime::ImageObservation{std::string{catalog.game[0]}, image ? std::optional{image} : std::nullopt}};
    auto session = fc::runtime::resolve_patches(std::move(admitted), catalog.profiles, FC_ROLE_CLIENT);
    fc::runtime::configure_patches(session, observations, {directory.path});
    return session;
}
} // namespace

TEST_CASE("scalar and finite map keys round trip while prefixed plugin filenames coexist with runtime configuration") {
    Directory directory;
    Catalog catalog;
    const std::string runtime_input = "[FusionCutter]\nLogLevel=Info\nMaxTraceSizeMB=0\n";
    directory.write("FusionCutter.ini", runtime_input);

    for (const auto id : {"CON", "FusionCutter"}) {
        CAPTURE(id);
        auto definition =
            fc::Plugin{.id = id,
                       .version = "1",
                       .patches = {fc::Patch{.id = "patch",
                                             .enabled = true,
                                             .settings = fc::settings<Names>({fc::setting("con", &Names::device, "Device."),
                                                                              fc::setting("camera.fov", &Names::fov, "View angle."),
                                                                              fc::setting("path/color", &Names::paths, "Color.")}),
                                             .variants = {fc::variant<SettingsHandler<Names>>({"Game.A"})}}}};
        fc::detail::PluginStorage storage{std::move(definition)};
        const auto filename = "FC." + std::string{id} + ".ini";

        auto generated = capture(storage, catalog, directory);
        const auto ini = fc::runtime::parse_ini(directory.read(filename), filename);
        REQUIRE(ini);

        directory.write(filename, "[patch]\ncon=2\ncamera.fov=110\npath/color=green\n");
        auto chosen = capture(storage, catalog, directory);
        REQUIRE(fc::runtime::construct_handler(*chosen.patches[0], nullptr));
        CHECK(SettingsHandler<Names>::observed->device == 2);
        CHECK(SettingsHandler<Names>::observed->fov == 110);
        CHECK(SettingsHandler<Names>::observed->paths.find("PATH/COLOR") == "green");
        CHECK(directory.read("FusionCutter.ini") == runtime_input);
    }

    CHECK(std::filesystem::exists(directory.path / "FC.CON.ini"));
    CHECK(std::filesystem::exists(directory.path / "FC.FusionCutter.ini"));
    const auto runtime = fc::runtime::load_runtime_configuration({directory.path});
    REQUIRE(runtime);
    CHECK(runtime->log_level == FC_LOG_INFO);
    CHECK(runtime->trace_file_bytes == 0);
    CHECK(runtime->warnings.empty());
    CHECK(directory.read("FusionCutter.ini") == runtime_input);
}

TEST_CASE("waiting alternatives leave differing defaults unset across repeated loads and later selection") {
    Directory directory;
    Catalog catalog;
    catalog.game[0] = "game.dll";
    fc::detail::PluginStorage storage{alternative_plugin()};

    auto waiting = capture_alternatives(storage, catalog, directory);

    REQUIRE(waiting.patches[0]->state == fc::runtime::PatchState::Waiting);

    // Only the common default becomes an assignment; differing defaults generate an unset commented key.
    const auto original = directory.read("FC.Example.ini");
    const auto generated = fc::runtime::parse_ini(original, "FC.Example.ini");
    REQUIRE(generated);
    CHECK(original.find("; limit =\n") != std::string::npos);
    CHECK(original.find("shared = 7\n") != std::string::npos);
    CHECK(std::ranges::none_of(generated->entries, [](const auto &entry) {
        return entry.key == "limit";
    }));
    CHECK(waiting.plugins[0]->configuration->warnings.empty());

    // Recognizing the unset comment prevents repeated capture from appending it again.
    auto repeated = capture_alternatives(storage, catalog, directory);
    CHECK(directory.read("FC.Example.ini") == original);

    // A later launch selects B without inheriting A's default as a generated override.
    auto selected = capture_alternatives(storage, catalog, directory, &catalog.profiles[1]);
    REQUIRE(fc::runtime::construct_handler(*selected.patches[0], nullptr));
    CHECK(SettingsHandler<DefaultB>::observed->limit == 20);
    CHECK(directory.read("FC.Example.ini") == original);

    directory.write("FC.Example.ini", "[patch]\nlimit=35\n");

    auto explicit_value = capture_alternatives(storage, catalog, directory, &catalog.profiles[1]);
    REQUIRE(fc::runtime::construct_handler(*explicit_value.patches[0], nullptr));

    CHECK(SettingsHandler<DefaultB>::observed->limit == 35);
}

#if !defined(_WIN64)
TEST_CASE("Core leaves BF2 defaults out of BF1 configuration and preserves existing user entries") {
    Directory directory;
    const auto catalog = fc::runtime::known_image_profiles();
    const auto profile = fc::runtime::find_image_profile("BF1.Steam.5F243336", catalog);
    REQUIRE(profile);
    std::vector<fc::runtime::ImageObservation> observations;
    for (const auto &candidate : catalog) {
        for (const auto filename : candidate.filenames) {
            if (std::ranges::none_of(observations, [&](const auto &value) {
                    return fc::detail::equal_id(value.filename, filename);
                })) {
                observations.push_back({std::string{filename}, std::nullopt});
            }
        }
    }
    for (auto &value : observations) {
        if (value.filename == (*profile)->filenames.front()) {
            value.outcome = *profile;
            value.executable = true;
        }
    }

    std::string existing;
    SECTION("a fresh launch leaves the Core INI absent") {}
    SECTION("an existing INI keeps BF2 settings and unrelated user keys") {
        existing = "; saved\n[UnlockFrameRate]\nMaxFrameRate=144\n[Other]\ncustom=keep\n";
        directory.write("FC.Core.ini", existing);
    }

    auto admitted = fc::runtime::admit_definitions({{"Core", fc::core_plugin::core_plugin()}});
    REQUIRE(admitted.diagnostics.empty());
    auto session = fc::runtime::resolve_patches(std::move(admitted), catalog, FC_ROLE_CLIENT);
    fc::runtime::configure_patches(session, observations, {directory.path});

    REQUIRE(session.plugins.size() == 1);
    REQUIRE(session.plugins[0]->configuration);
    CHECK(session.plugins[0]->configuration->variants.empty());
    if (existing.empty()) {
        CHECK_FALSE(std::filesystem::exists(directory.path / "FC.Core.ini"));
    } else {
        CHECK(directory.read("FC.Core.ini") == existing);
    }
}
#endif
