#include "catalog.hpp"
#include "configuration.hpp"

#include <FusionCutter/patch.hpp>

#include <Windows.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

enum class TestMode {
    Automatic,
    Manual,
};

constexpr std::array kTestModeChoices{
    fusioncutter::ChoiceValue{"Automatic", TestMode::Automatic},
    fusioncutter::ChoiceValue{"Manual", TestMode::Manual},
};
constexpr std::array kUnitBindings{
    fusioncutter::KeyedStringSetting{"A", "Crouch", "Unit A action", 32},
    fusioncutter::KeyedStringSetting{"B", "Roll", {}, 32},
};
static_assert(fusioncutter::choice_name(TestMode::Manual, kTestModeChoices) == "Manual");

struct TestSettings {
    int count;
    float ratio;
    bool enabled;
    std::string name;
    TestMode mode;
    fusioncutter::KeyedStrings unit_bindings;
};

[[nodiscard]] std::expected<void, fusioncutter::OutcomeReason> validate_settings(TestSettings& settings) {
    if (settings.enabled && settings.name.empty()) {
        return std::unexpected(fusioncutter::OutcomeReason{"enabled settings require a name", {}, {}});
    }
    return {};
}

[[nodiscard]] fusioncutter::SettingsDefinition test_settings_definition() {
    return fusioncutter::SettingsDefinition::from(fusioncutter::SettingsSchema<TestSettings>{
        .values =
            {
                fusioncutter::setting("Count", &TestSettings::count, 3).range(1, 8),
                fusioncutter::setting("Ratio", &TestSettings::ratio, 0.5F).range(0.0F, 1.0F),
                fusioncutter::setting("Enabled", &TestSettings::enabled, true),
                fusioncutter::setting("Name", &TestSettings::name, std::string("Player")).max_length(16),
                fusioncutter::choice("Mode", &TestSettings::mode, TestMode::Automatic, kTestModeChoices),
            },
        .groups =
            {
                fusioncutter::keyed_string_group("Unit", &TestSettings::unit_bindings, kUnitBindings),
            },
        .validate = validate_settings,
    });
}

class ConfiguredPatch final : public fusioncutter::RuntimePatch {
  public:
    ConfiguredPatch(TestSettings settings, const fusioncutter::TargetContext& target)
        : settings_(std::move(settings)), layout_(target.layout) {}

    void build_plan(fusioncutter::PatchPlan&) override {}

    [[nodiscard]] const TestSettings& settings() const noexcept {
        return settings_;
    }

    [[nodiscard]] fusioncutter::TargetLayout layout() const noexcept {
        return layout_;
    }

  private:
    TestSettings settings_;
    fusioncutter::TargetLayout layout_;
};

class RuntimeService final : public fusioncutter::RuntimeOnlyPatch, public fusioncutter::Updatable {
  public:
    explicit RuntimeService(const fusioncutter::TargetContext&) {}

    void update() noexcept override {
        ++updates_;
    }

    [[nodiscard]] int updates() const noexcept {
        return updates_;
    }

  private:
    int updates_{};
};

template <typename PatchType, typename Settings = fusioncutter::NoSettings>
[[nodiscard]] fusioncutter::PatchVariant
test_variant(fusioncutter::TargetLayout layout, fusioncutter::HostRole role, fusioncutter::TargetImage image,
             fusioncutter::ImageTiming image_timing = fusioncutter::ImageTiming::Startup,
             fusioncutter::StartupFailurePolicy failure_policy = fusioncutter::StartupFailurePolicy::Local) {
    return {layout,       role,           image,
            image_timing, failure_policy, fusioncutter::patch_detail::patch_factory<PatchType, Settings>()};
}

[[nodiscard]] fusioncutter::TargetContext steam_client_target() {
    return {
        fusioncutter::TargetLayout::SteamRetail,
        fusioncutter::HostRole::Client,
        {fusioncutter::TargetImage::Game, fusioncutter::Architecture::X86, 0x00400000, 0x00800000},
    };
}

[[nodiscard]] fusioncutter::TargetContext steam_server_target() {
    auto target = steam_client_target();
    target.role = fusioncutter::HostRole::Server;
    return target;
}

class TemporaryDirectory {
  public:
    TemporaryDirectory() {
        static std::atomic_uint64_t sequence;
        path_ = std::filesystem::temp_directory_path() /
                ("FusionCutter-config-tests-" + std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)));
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

[[nodiscard]] std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    REQUIRE(input);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

[[nodiscard]] fusioncutter::PatchDefinition patch_definition(bool enabled, bool configurable,
                                                             fusioncutter::SettingsDefinition settings,
                                                             std::span<const fusioncutter::PatchVariant> variants,
                                                             std::string_view category = "Gameplay") {
    return {
        .name = "Test Patch",
        .enabled = enabled,
        .configurable = configurable,
        .category = {category, 10},
        .description = configurable ? "Test patch" : std::string_view{},
        .settings = std::move(settings),
        .depends_on = {},
        .includes = {},
        .variants = variants,
    };
}

[[nodiscard]] const fusioncutter::config::PatchToggle* find_toggle(const fusioncutter::config::Configuration& config,
                                                                   fusioncutter::PatchId patch_id) {
    const auto toggles = config.patch_toggles();
    const auto found = std::ranges::find(toggles, patch_id, &fusioncutter::config::PatchToggle::patch_id);
    return found == toggles.end() ? nullptr : &*found;
}

} // namespace

TEST_CASE("Typed settings resolve defaults, overrides, groups, and validation", "[core][config]") {
    const auto definition = test_settings_definition();
    REQUIRE(definition.validate_metadata().has_value());

    auto settings = definition.make_defaults();
    const auto count = definition.find({}, "count");
    const auto mode = definition.find({}, "MODE");
    const auto unit_a = definition.find("unit", "a");
    REQUIRE(count.has_value());
    REQUIRE(mode.has_value());
    REQUIRE(unit_a.has_value());

    CHECK(definition.apply(settings, *count, "7").has_value());
    CHECK(definition.apply(settings, *mode, "manual").has_value());
    CHECK(definition.apply(settings, *unit_a, "Jump").has_value());
    CHECK_FALSE(definition.apply(settings, *count, "12").has_value());
    CHECK(definition.validate(settings).has_value());

    const auto variant = test_variant<ConfiguredPatch, TestSettings>(
        fusioncutter::TargetLayout::SteamRetail, fusioncutter::HostRole::Client, fusioncutter::TargetImage::Game);
    auto instance = variant.factory.construct(std::move(settings), steam_client_target());
    auto* patch = dynamic_cast<ConfiguredPatch*>(std::get<std::unique_ptr<fusioncutter::Patch>>(instance).get());
    REQUIRE(patch != nullptr);
    CHECK(patch->settings().count == 7);
    CHECK(patch->settings().mode == TestMode::Manual);
    CHECK(patch->settings().unit_bindings.value("A") == "Jump");
    CHECK(patch->layout() == fusioncutter::TargetLayout::SteamRetail);
}

TEST_CASE("Variant settings keep role-specific configuration with one patch identity", "[core][config]") {
    const auto server_settings = test_settings_definition();
    const std::array variants{
        test_variant<RuntimeService>(fusioncutter::TargetLayout::SteamRetail, fusioncutter::HostRole::Client,
                                     fusioncutter::TargetImage::Game),
        fusioncutter::PatchVariant{
            fusioncutter::TargetLayout::SteamRetail,
            fusioncutter::HostRole::Server,
            fusioncutter::TargetImage::Game,
            fusioncutter::ImageTiming::Startup,
            fusioncutter::StartupFailurePolicy::Local,
            fusioncutter::patch_detail::patch_factory<ConfiguredPatch, TestSettings>(),
            server_settings,
        },
    };
    auto catalog = fusioncutter::catalog::initialize_catalog(
        {fusioncutter::catalog::catalog_entry("DirectTransport", patch_definition(true, true, {}, variants, "Network"),
                                              {true, false, true, true})},
        {fusioncutter::Architecture::X86, true, true});
    REQUIRE(catalog.has_value());

    TemporaryDirectory directory;
    const auto client_patches = fusioncutter::catalog::configurable_patches(*catalog, steam_client_target());
    auto client = fusioncutter::config::load_configuration(directory.file("Client.ini"), client_patches);
    REQUIRE(client.has_value());
    CHECK_FALSE(read_file(directory.file("Client.ini")).contains("[DirectTransport]"));

    const auto server_patches = fusioncutter::catalog::configurable_patches(*catalog, steam_server_target());
    auto server = fusioncutter::config::load_configuration(directory.file("Server.ini"), server_patches);
    REQUIRE(server.has_value());
    CHECK(read_file(directory.file("Server.ini")).contains("[DirectTransport]"));
    auto resolved = server->resolve_settings("DirectTransport");
    REQUIRE(resolved.has_value());
    CHECK(std::move(*resolved).take<TestSettings>().count == 3);
}

TEST_CASE("Environment helpers distinguish absent, valid, and malformed patch inputs", "[core][config]") {
    constexpr auto name = "FC_TEST_PATCH_ENVIRONMENT";
    struct Cleanup {
        ~Cleanup() {
            SetEnvironmentVariableA(name, nullptr);
        }
        const char* name;
    } cleanup{name};
    SetEnvironmentVariableA(name, nullptr);

    auto absent = fusioncutter::read_environment_choice(name, kTestModeChoices);
    REQUIRE(absent.has_value());
    CHECK_FALSE(absent->has_value());

    REQUIRE(SetEnvironmentVariableA(name, "manual"));
    auto choice =
        fusioncutter::read_environment_choice(name, {fusioncutter::ChoiceValue{"Automatic", TestMode::Automatic},
                                                     fusioncutter::ChoiceValue{"Manual", TestMode::Manual}});
    REQUIRE(choice.has_value());
    CHECK(*choice == TestMode::Manual);

    REQUIRE(SetEnvironmentVariableA(name, "24"));
    auto integer = fusioncutter::read_environment_value<int>(name);
    REQUIRE(integer.has_value());
    CHECK(*integer == 24);

    REQUIRE(SetEnvironmentVariableA(name, "Unknown"));
    const auto invalid = fusioncutter::read_environment_choice(name, kTestModeChoices);
    REQUIRE_FALSE(invalid.has_value());
    CHECK(invalid.error().message.contains("Automatic, Manual"));

    const std::string oversized(4097, 'A');
    REQUIRE(SetEnvironmentVariableA(name, oversized.c_str()));
    const auto too_long = fusioncutter::read_environment_value<std::string>(name);
    REQUIRE_FALSE(too_long.has_value());
    CHECK(too_long.error().message.contains("4096"));
}

TEST_CASE("Configuration generation round trips only applicable configurable patches", "[core][config]") {
    const std::array configured_variants{
        test_variant<ConfiguredPatch, TestSettings>(fusioncutter::TargetLayout::SteamRetail,
                                                    fusioncutter::HostRole::Client, fusioncutter::TargetImage::Game),
    };
    const std::array late_variants{
        test_variant<ConfiguredPatch, TestSettings>(
            fusioncutter::TargetLayout::SteamRetail, fusioncutter::HostRole::Client,
            fusioncutter::TargetImage::GalaxyPeer, fusioncutter::ImageTiming::OneShotLate),
    };
    const std::array wrong_layout_variants{
        test_variant<ConfiguredPatch, TestSettings>(fusioncutter::TargetLayout::GOGRetail,
                                                    fusioncutter::HostRole::Client, fusioncutter::TargetImage::Game),
    };
    const std::array internal_variants{
        test_variant<RuntimeService>(fusioncutter::TargetLayout::SteamRetail, fusioncutter::HostRole::Client,
                                     fusioncutter::TargetImage::Game),
    };

    fusioncutter::catalog::Catalog catalog({
        fusioncutter::catalog::catalog_entry(
            "AimAssist", patch_definition(true, true, test_settings_definition(), configured_variants),
            {true, false, true, false}),
        fusioncutter::catalog::catalog_entry("InternalSupport", patch_definition(true, false, {}, internal_variants),
                                             {true, false, true, false}),
        fusioncutter::catalog::catalog_entry(
            "LateFeature", patch_definition(false, true, test_settings_definition(), late_variants, "Network"),
            {true, false, true, false}),
        fusioncutter::catalog::catalog_entry(
            "WrongLayout", patch_definition(true, true, test_settings_definition(), wrong_layout_variants),
            {true, false, true, false}),
    });

    const auto patches = fusioncutter::catalog::configurable_patches(catalog, steam_client_target());
    REQUIRE(patches.size() == 2);
    CHECK(std::string(patches[0].id) == "AimAssist");
    CHECK(std::string(patches[1].id) == "LateFeature");

    TemporaryDirectory directory;
    const auto path = directory.file("FusionCutter.ini");
    auto generated = fusioncutter::config::load_configuration(path, patches);
    REQUIRE(generated.has_value());
    CHECK(generated->file_created());
    CHECK_FALSE(generated->output_error().has_value());

    const auto generated_text = read_file(path);
    CHECK(generated_text.starts_with("[Logging]\r\nLevel="));
    CHECK(generated_text.contains("AimAssist=1\r\n"));
    CHECK(generated_text.contains("LateFeature=0\r\n"));
    CHECK(generated_text.contains("[AimAssist]\r\n"));
    CHECK(generated_text.contains("[AimAssist.Unit]\r\n"));
    CHECK_FALSE(generated_text.contains("InternalSupport"));
    CHECK_FALSE(generated_text.contains("WrongLayout"));

    auto loaded = fusioncutter::config::load_configuration(path, patches);
    REQUIRE(loaded.has_value());
    CHECK_FALSE(loaded->file_created());
    CHECK(read_file(path) == generated_text);

    const auto* aim_toggle = find_toggle(*loaded, "AimAssist");
    const auto* late_toggle = find_toggle(*loaded, "LateFeature");
    REQUIRE(aim_toggle != nullptr);
    REQUIRE(late_toggle != nullptr);
    CHECK(aim_toggle->override_value == true);
    CHECK(late_toggle->override_value == false);

    auto resolved = loaded->resolve_settings("AimAssist");
    REQUIRE(resolved.has_value());
    const auto settings = std::move(*resolved).take<TestSettings>();
    CHECK(settings.count == 3);
    CHECK(settings.unit_bindings.value("A") == "Crouch");
}

TEST_CASE("Existing configuration applies the last recognized value without rewriting the file", "[core][config]") {
    const std::array variants{
        test_variant<ConfiguredPatch, TestSettings>(fusioncutter::TargetLayout::SteamRetail,
                                                    fusioncutter::HostRole::Client, fusioncutter::TargetImage::Game),
    };
    auto definition = patch_definition(true, true, test_settings_definition(), variants);
    const std::array patches{fusioncutter::config::ApplicablePatch{"AimAssist", &definition, &definition.settings}};
    constexpr std::string_view content = "[Logging]\r\n"
                                         "Level=debug\r\n"
                                         "\r\n"
                                         "[Patches]\r\n"
                                         "AimAssist=off\r\n"
                                         "aimassist=ON\r\n"
                                         "UnknownPatch=1\r\n"
                                         "\r\n"
                                         "[aimassist]\r\n"
                                         "Count=7\r\n"
                                         "Mode=manual\r\n"
                                         "UnknownSetting=ignored\r\n"
                                         "\r\n"
                                         "[AimAssist.Unit]\r\n"
                                         "A=Jump\r\n"
                                         "\r\n"
                                         "[Unused]\r\n"
                                         "Value=ignored\r\n";

    TemporaryDirectory directory;
    const auto path = directory.file("FusionCutter.ini");
    write_file(path, content);

    auto loaded = fusioncutter::config::load_configuration(path, patches);
    REQUIRE(loaded.has_value());
    CHECK(read_file(path) == std::string(content));
    CHECK(loaded->diagnostics().size() == 4);
    CHECK(loaded->log_level() == fusioncutter::LogLevel::Debug);

    const auto* toggle = find_toggle(*loaded, "AimAssist");
    REQUIRE(toggle != nullptr);
    CHECK(toggle->override_value == true);
    CHECK_FALSE(toggle->error.has_value());

    auto resolved = loaded->resolve_settings("AimAssist");
    REQUIRE(resolved.has_value());
    const auto settings = std::move(*resolved).take<TestSettings>();
    CHECK(settings.count == 7);
    CHECK(settings.mode == TestMode::Manual);
    CHECK(settings.unit_bindings.value("A") == "Jump");
}

TEST_CASE("Unsafe existing configuration fails before modifying its contents", "[core][config]") {
    const std::array variants{
        test_variant<ConfiguredPatch, TestSettings>(fusioncutter::TargetLayout::SteamRetail,
                                                    fusioncutter::HostRole::Client, fusioncutter::TargetImage::Game),
    };
    auto definition = patch_definition(true, true, test_settings_definition(), variants);
    const std::array patches{fusioncutter::config::ApplicablePatch{"AimAssist", &definition, &definition.settings}};

    TemporaryDirectory directory;
    const std::array cases{
        std::pair{directory.file("Malformed.ini"), std::string("[Patches\r\nAimAssist=1\r\n")},
        std::pair{directory.file("EmbeddedNul.ini"), std::string("[Patches]\r\nAim\0Assist=1\r\n", 26)},
        std::pair{directory.file("Oversized.ini"), std::string(256 * 1024 + 1, 'A')},
    };

    for (const auto& [path, content] : cases) {
        DYNAMIC_SECTION(path.filename().string()) {
            write_file(path, content);
            const auto loaded = fusioncutter::config::load_configuration(path, patches);
            CHECK_FALSE(loaded.has_value());
            CHECK(read_file(path) == content);
        }
    }
}

TEST_CASE("A missing-file write failure keeps compiled defaults available", "[core][config]") {
    const std::array variants{
        test_variant<ConfiguredPatch, TestSettings>(fusioncutter::TargetLayout::SteamRetail,
                                                    fusioncutter::HostRole::Client, fusioncutter::TargetImage::Game),
    };
    auto definition = patch_definition(true, true, test_settings_definition(), variants);
    const std::array patches{fusioncutter::config::ApplicablePatch{"AimAssist", &definition, &definition.settings}};

    TemporaryDirectory directory;
    const auto path = directory.file("MissingParent") / "FusionCutter.ini";
    auto loaded = fusioncutter::config::load_configuration(path, patches);
    REQUIRE(loaded.has_value());
    CHECK_FALSE(loaded->file_created());
    CHECK(loaded->output_error().has_value());

    const auto* toggle = find_toggle(*loaded, "AimAssist");
    REQUIRE(toggle != nullptr);
    CHECK_FALSE(toggle->override_value.has_value());

    auto resolved = loaded->resolve_settings("AimAssist");
    REQUIRE(resolved.has_value());
    const auto settings = std::move(*resolved).take<TestSettings>();
    CHECK(settings.count == 3);
}
