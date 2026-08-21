#include "configuration.hpp"

#include <catch2/catch_test_macros.hpp>

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

// Gives each filesystem test exclusive configuration state and removes it even after assertion unwinding.
class TemporaryDirectory {
  public:
    TemporaryDirectory() {
        static std::atomic_uint32_t sequence;
        path_ = std::filesystem::temp_directory_path() /
                ("FusionCutter-config-" + std::to_string(GetCurrentProcessId()) + "-" +
                 std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)));
        std::filesystem::create_directories(path_ / "config");
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

  private:
    std::filesystem::path path_;
};

// Compact fixture builders create the smallest selected patch and setting records accepted by configuration.
[[nodiscard]] fc::catalog::PatchDefinitionRecord patch(std::string id, std::string category = {}) {
    fc::catalog::PatchDefinitionRecord result;
    result.id = std::move(id);
    result.category = std::move(category);
    result.configurable = FC_TRUE;
    result.enabled = FC_TRUE;
    result.supports.emplace_back();
    result.selected_support = 0;
    return result;
}

[[nodiscard]] fc::catalog::SettingDefinitionRecord setting(FC_SettingType type) {
    fc::catalog::SettingDefinitionRecord result;
    result.key = "Value";
    result.type = type;
    result.has_range = FC_FALSE;
    return result;
}

// Writes exact fixture bytes so parser tests do not inherit newline or encoding normalization from a helper library.
void write_file(const std::filesystem::path& path, std::string_view content) {
    std::ofstream output{path, std::ios::binary};
    REQUIRE(output.is_open());
    output.write(content.data(), static_cast<std::streamsize>(content.size()));
    REQUIRE(output.good());
}

} // namespace

TEST_CASE("typed setting conversion covers every native kind and rejects invalid authoritative text") {
    // Boundary spellings prove each numeric width parses completely and round-trips through generated defaults.
    struct Row {
        FC_SettingType type;
        std::string_view text;
    };
    constexpr Row rows[]{{FC_SETTING_SIGNED_8, "-128"},          {FC_SETTING_SIGNED_16, "-32768"},
                         {FC_SETTING_SIGNED_32, "-2147483648"},  {FC_SETTING_SIGNED_64, "-9223372036854775808"},
                         {FC_SETTING_UNSIGNED_8, "255"},         {FC_SETTING_UNSIGNED_16, "65535"},
                         {FC_SETTING_UNSIGNED_32, "4294967295"}, {FC_SETTING_UNSIGNED_64, "18446744073709551615"},
                         {FC_SETTING_FLOAT_32, "1.25"},          {FC_SETTING_FLOAT_64, "1.125"}};
    for (const auto& row : rows) {
        INFO("Setting type: " << row.type);
        auto definition = setting(row.type);
        const auto parsed = fc::config::resolve_setting_value(definition, row.text);
        REQUIRE(parsed.has_value());
        definition.default_value = parsed->native_value();
        CHECK(
            fc::config::resolve_setting_value(definition, fc::config::format_setting_default(definition)).has_value());
    }

    // Non-numeric domains exercise their accepted aliases, ownership, limits, and canonical default spelling.
    auto boolean = setting(FC_SETTING_BOOLEAN);
    boolean.default_value.boolean_value = FC_TRUE;
    CHECK(fc::config::resolve_setting_value(boolean, fc::config::format_setting_default(boolean)).has_value());
    CHECK(fc::config::resolve_setting_value(boolean, "ON").has_value());
    CHECK_FALSE(fc::config::resolve_setting_value(setting(FC_SETTING_BOOLEAN), "enabled").has_value());

    auto string_setting = setting(FC_SETTING_STRING);
    string_setting.max_length = 4;
    string_setting.default_string = "test";
    CHECK(fc::config::resolve_setting_value(string_setting, "test").has_value());
    CHECK(fc::config::resolve_setting_value(string_setting, fc::config::format_setting_default(string_setting))
              .has_value());
    CHECK_FALSE(fc::config::resolve_setting_value(string_setting, "large").has_value());

    auto choice = setting(FC_SETTING_CHOICE);
    choice.choices = {"Disabled", "Required"};
    choice.default_value.choice_index = 1;
    CHECK(fc::config::resolve_setting_value(choice, "required")->native_value().choice_index == 1);
    CHECK(fc::config::resolve_setting_value(choice, fc::config::format_setting_default(choice)).has_value());
    CHECK_FALSE(fc::config::resolve_setting_value(choice, "Unknown").has_value());
    CHECK_FALSE(fc::config::resolve_setting_value(setting(FC_SETTING_FLOAT_32), "inf").has_value());

    auto ranged = setting(FC_SETTING_SIGNED_8);
    ranged.has_range = FC_TRUE;
    ranged.minimum.signed_value = -5;
    ranged.maximum.signed_value = 5;
    CHECK_FALSE(fc::config::resolve_setting_value(ranged, "6").has_value());
}

TEST_CASE("generation uses the one presentation order and preserves schema declaration order") {
    fc::catalog::PluginDefinitionRecord plugin;
    plugin.id = "Core";
    plugin.categories = {{.id = "Zulu"}, {.id = "Alpha", .has_order = FC_TRUE, .order = 4}};
    plugin.patches.push_back(patch("ZuluPatch", "Zulu"));
    plugin.patches.push_back(patch("GeneralPatch"));
    auto alpha_patch = patch("AlphaPatch");
    auto base = setting(FC_SETTING_BOOLEAN);
    base.key = "Enabled";
    base.default_value.boolean_value = FC_TRUE;
    auto named = setting(FC_SETTING_UNSIGNED_16);
    named.section = "Network";
    named.key = "Port";
    named.default_value.unsigned_value = 3658;
    alpha_patch.settings = {base, named};
    plugin.patches.push_back(std::move(alpha_patch));
    plugin.groups.push_back({.id = "AlphaGroup",
                             .members = {"AlphaPatch"},
                             .configurable = FC_TRUE,
                             .enabled = FC_FALSE,
                             .category = "Alpha"});

    // The literal makes ordering across categories and settings sections observable in one assertion.
    const auto rendered = fc::config::render_configuration(plugin, true);
    REQUIRE(rendered.has_value());
    const std::string expected =
        "[FusionCutter]\r\nLogLevel=Debug\r\nMaxTraceSizeMB=512\r\n\r\n[General]\r\nAlphaPatch=true\r\n"
        "GeneralPatch=true\r\n"
        "\r\n[Alpha]\r\nAlphaGroup=false\r\n\r\n[Zulu]\r\nZuluPatch=true\r\n\r\n[AlphaPatch]\r\n"
        "Enabled=true\r\n\r\n[AlphaPatch.Network]\r\nPort=3658\r\n";
#if defined(NDEBUG)
    auto optimized_expected = expected;
    optimized_expected.replace(optimized_expected.find("LogLevel=Debug"), 14, "LogLevel=Error");
    CHECK(*rendered == optimized_expected);
#else
    CHECK(*rendered == expected);
#endif
}

TEST_CASE("existing files are read once, the last duplicate wins, and invalid files are rejected in full") {
    TemporaryDirectory directory;
    fc::catalog::PluginDefinitionRecord plugin;
    plugin.id = "ConfigProbe";
    auto configurable = patch("ConfigPatch");
    auto value = setting(FC_SETTING_UNSIGNED_16);
    value.key = "Port";
    value.default_value.unsigned_value = 3658;
    configurable.settings.push_back(value);
    plugin.patches.push_back(std::move(configurable));
    const auto path = directory.path() / "config" / "FC.ConfigProbe.ini";
    write_file(path, "[General]\nConfigPatch=false\nConfigPatch=on\nUnknown=true\n\n[ConfigPatch]\nPort:1234\n");

    // First prove recoverable entry diagnostics, then replace the same file with a fatal whole-file syntax error.
    auto loaded = fc::config::load_configuration(plugin, false, fc::config::ConfigurationPaths{directory.path()});
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->configuration.patches.front().toggle.has_value());
    CHECK(loaded->configuration.patches.front().toggle->value == "on");
    REQUIRE(loaded->configuration.patches.front().settings.front().ini.has_value());
    CHECK(loaded->configuration.patches.front().settings.front().ini->value == "1234");
    CHECK(loaded->configuration.diagnostics.size() == 2);

    write_file(path, "[General]\rConfigPatch=true\n");
    CHECK_FALSE(
        fc::config::load_configuration(plugin, false, fc::config::ConfigurationPaths{directory.path()}).has_value());
}

TEST_CASE("missing files use exclusive creation and nonparticipating plugins stay untouched") {
    TemporaryDirectory directory;
    fc::catalog::PluginDefinitionRecord plugin;
    plugin.id = "GeneratedProbe";
    plugin.patches.push_back(patch("GeneratedPatch"));
    auto generated = fc::config::load_configuration(plugin, false, fc::config::ConfigurationPaths{directory.path()});
    REQUIRE(generated.has_value());
    CHECK(generated->configuration.file_created);
    CHECK(std::filesystem::exists(directory.path() / "config" / "FC.GeneratedProbe.ini"));

    // With no selected support, the dormant schema contributes neither toggles nor settings and needs no file.
    fc::catalog::PluginDefinitionRecord dormant;
    dormant.id = "DormantProbe";
    dormant.patches.push_back(patch("DormantPatch"));
    dormant.patches.front().selected_support.reset();
    auto ignored = fc::config::load_configuration(dormant, false, fc::config::ConfigurationPaths{directory.path()});
    REQUIRE(ignored.has_value());
    CHECK_FALSE(ignored->configuration.file_expected);
    CHECK_FALSE(std::filesystem::exists(directory.path() / "config" / "FC.DormantProbe.ini"));
}

TEST_CASE("concurrent missing-file generation preserves one exclusive winner without overwriting it") {
    TemporaryDirectory directory;
    std::array<fc::catalog::PluginDefinitionRecord, 2> schemas;
    for (auto& schema : schemas) {
        schema.id = "ConcurrentProbe";
    }
    schemas[0].patches.push_back(patch("AlphaPatch"));
    schemas[1].patches.push_back(patch("BravoPatch"));

    // Distinct schemas make an overwrite observable even though both creators target the same plugin-owned file.
    std::array<std::expected<fc::config::ConfigurationLoadResult, std::string>, 2> results;
    std::atomic_bool start{};
    std::array<std::thread, 2> workers;
    for (std::size_t index = 0; index < workers.size(); ++index) {
        workers[index] = std::thread([&, index] {
            start.wait(false, std::memory_order_acquire);
            results[index] =
                fc::config::load_configuration(schemas[index], false, fc::config::ConfigurationPaths{directory.path()});
        });
    }
    start.store(true, std::memory_order_release);
    start.notify_all();
    for (auto& worker : workers) {
        worker.join();
    }

    REQUIRE(results[0].has_value());
    REQUIRE(results[1].has_value());
    const auto first_created = results[0]->configuration.file_created;
    const auto second_created = results[1]->configuration.file_created;
    REQUIRE(first_created != second_created);
    const auto winner = first_created ? 0U : 1U;
    const auto expected = fc::config::render_configuration(schemas[winner], false);
    REQUIRE(expected.has_value());

    // The loser must parse the winner's complete file rather than truncate or regenerate the destination.
    std::ifstream input{directory.path() / "config" / "FC.ConcurrentProbe.ini", std::ios::binary};
    REQUIRE(input.is_open());
    const std::string content{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    CHECK(content == *expected);
}

TEST_CASE("framework settings fall back individually and environment values are captured once") {
    // Two invalid framework scalars must diagnose independently while preserving both compiled defaults.
    TemporaryDirectory directory;
    write_file(directory.path() / "config" / "FC.Core.ini", "[FusionCutter]\nLogLevel=Verbose\nMaxTraceSizeMB=5000\n");
    fc::catalog::PluginDefinitionRecord core;
    core.id = "Core";
    auto core_configuration =
        fc::config::load_configuration(core, true, fc::config::ConfigurationPaths{directory.path()});
    REQUIRE(core_configuration.has_value());
    REQUIRE(core_configuration->framework.has_value());
#if defined(NDEBUG)
    CHECK(core_configuration->framework->log_level == FC_LOG_ERROR);
#else
    CHECK(core_configuration->framework->log_level == FC_LOG_DEBUG);
#endif
    CHECK(core_configuration->framework->max_trace_size_mb == 512);
    CHECK(core_configuration->configuration.diagnostics.size() == 2);

    // Removing the process variable after loading proves the snapshot owns the one captured value.
    constexpr auto environment_name = "FC_STAGE3_CAPTURE_PROBE";
    REQUIRE(SetEnvironmentVariableA(environment_name, "73") != 0);
    fc::catalog::PluginDefinitionRecord plugin;
    plugin.id = "EnvironmentProbe";
    auto configurable = patch("EnvironmentPatch");
    configurable.configurable = FC_FALSE;
    auto value = setting(FC_SETTING_UNSIGNED_8);
    value.environment = environment_name;
    configurable.settings.push_back(value);
    plugin.patches.push_back(std::move(configurable));
    auto captured = fc::config::load_configuration(plugin, false, fc::config::ConfigurationPaths{directory.path()});
    REQUIRE(SetEnvironmentVariableA(environment_name, nullptr) != 0);
    REQUIRE(captured.has_value());
    REQUIRE(captured->configuration.patches.front().settings.front().environment.has_value());
    CHECK(*captured->configuration.patches.front().settings.front().environment == "73");
}

TEST_CASE("an empty selected support replacement suppresses common settings participation") {
    TemporaryDirectory directory;
    fc::catalog::PluginDefinitionRecord plugin;
    plugin.id = "ReplacementProbe";
    auto replaced = patch("ReplacementPatch");
    replaced.configurable = FC_FALSE;
    replaced.settings.push_back(setting(FC_SETTING_BOOLEAN));
    // An explicit empty support schema replaces, rather than inherits, the otherwise participating common schema.
    replaced.supports.front().has_settings = FC_TRUE;
    replaced.supports.front().settings.clear();
    plugin.patches.push_back(std::move(replaced));

    auto loaded = fc::config::load_configuration(plugin, false, fc::config::ConfigurationPaths{directory.path()});
    REQUIRE(loaded.has_value());
    CHECK_FALSE(loaded->configuration.file_expected);
    CHECK_FALSE(std::filesystem::exists(directory.path() / "config" / "FC.ReplacementProbe.ini"));
}

TEST_CASE("configuration diagnostics retain recoverable input within the bounded file") {
    TemporaryDirectory directory;
    fc::catalog::PluginDefinitionRecord plugin;
    plugin.id = "BoundedProbe";
    plugin.patches.push_back(patch("BoundedPatch"));
    // Every recoverable unknown key remains available to the common reporting path.
    std::string content = "[General]\nBoundedPatch=true\n";
    for (std::size_t index = 0; index < 40; ++index) {
        content += "Unknown" + std::to_string(index) + "=true\n";
    }
    const auto path = directory.path() / "config" / "FC.BoundedProbe.ini";
    write_file(path, content);
    auto loaded = fc::config::load_configuration(plugin, false, fc::config::ConfigurationPaths{directory.path()});
    REQUIRE(loaded.has_value());
    CHECK(loaded->configuration.diagnostics.size() == 40);

    // Whole-file size is an independent hard failure even when diagnostic retention is bounded.
    write_file(path, std::string(fc::config::kConfigurationFileByteCapacity + 1, 'x'));
    CHECK_FALSE(
        fc::config::load_configuration(plugin, false, fc::config::ConfigurationPaths{directory.path()}).has_value());
}
