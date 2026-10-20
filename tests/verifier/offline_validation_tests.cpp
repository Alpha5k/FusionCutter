#include "offline_validation.hpp"
#include "image_fixture.hpp"
#include "plugin.hpp"
#include "../test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <Windows.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace {

// Writes an INI fixture verbatim so tests can compare its bytes after validation.
void write_text(const std::filesystem::path &path, std::string_view text) {
    std::ofstream output{path, std::ios::binary};
    REQUIRE(output);
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    REQUIRE(output);
}

// Keeps the fixture loaded while reading its cumulative observations before and after validation.
struct FixtureModule {
    HMODULE module = LoadLibraryW(std::filesystem::path{FC_VERIFIER_FIXTURE}.c_str());
    ReadVerifierCounters read{};

    // Resolves the fixture's export for reading callback observations.
    FixtureModule() {
        REQUIRE(module);
        read = reinterpret_cast<ReadVerifierCounters>(GetProcAddress(module, "FC_VerifierCounters"));
        REQUIRE(read);
    }

    // Releases this reference; production admission keeps its own accepted DLL reference until process exit.
    ~FixtureModule() {
        FreeLibrary(module);
    }

    // Copies observations owned by the separately compiled plugin DLL.
    VerifierCounters counters() const {
        VerifierCounters value;
        read(&value);
        return value;
    }
};

} // namespace

TEST_CASE("Offline validation loads a DLL and constructs handlers with configured settings before planning", "[verifier][offline]") {
    fc::tests::TemporaryDirectory directory;
    const auto image = fc::tests::verifier::write_image(directory.path);
    const auto original = fc::tests::read_text(image);
    const auto ini = directory.path / "FC.VerifyFixture.ini";
    constexpr std::string_view contents = "[Ready]\nvalue=23\n";
    write_text(ini, contents);
    FixtureModule fixture;
    const auto before = fixture.counters();
    fc::validation::ValidationInput input;
    input.images.push_back({image});
    input.plugins.push_back(FC_VERIFIER_FIXTURE);
    input.config_directory = directory.path;

    const auto result = fc::validation::validate_offline(input);

    REQUIRE(result);
    const auto *ready = fc::validation::find_patch(*result, "verifyfixture.ready");
    REQUIRE(ready);
    CHECK(ready->state == fc::runtime::PatchState::Planning);
    REQUIRE(ready->plan);
    CHECK(ready->plan->operations().size() == 3);
    CHECK_FALSE(ready->plan->exposed());
    const auto after = fixture.counters();
    CHECK(after.ready_constructed == before.ready_constructed + 1);
    CHECK(after.ready_planned == before.ready_planned + 1);
    CHECK(after.ready_value == 23);
    CHECK(after.ready_observed == 0x31);
    CHECK(after.prepare == before.prepare);
    CHECK(after.activate == before.activate);
    CHECK(after.update == before.update);
    CHECK(fc::tests::read_text(image) == original);
    CHECK(fc::tests::read_text(ini) == contents);
    CHECK_FALSE(std::filesystem::exists(directory.path / "FC.Core.ini"));
    CHECK_FALSE(std::filesystem::exists(directory.path / "FusionCutter.ini"));
}

TEST_CASE("Offline validation reads runtime INI settings without changing files", "[verifier][offline]") {
    fc::tests::TemporaryDirectory directory;
    const auto image = fc::tests::verifier::write_image(directory.path);
    const auto config = directory.path / "config";
    REQUIRE(std::filesystem::create_directory(config));
    const auto ini = config / "FusionCutter.ini";
    fc::validation::ValidationInput input;
    input.images.push_back({image});
    input.config_directory = config;

    SECTION("An absent runtime INI selects defaults and is not created") {
        const auto result = fc::validation::validate_offline(input);
        REQUIRE(result);
        CHECK(result->runtime_settings.log_level == FC_LOG_WARNING);
        CHECK_FALSE(std::filesystem::exists(ini));
        CHECK_FALSE(std::filesystem::exists(config / "FC.Core.ini"));
    }

    SECTION("Configured runtime settings are read without changing the file") {
        constexpr std::string_view contents = "[FusionCutter]\nLogLevel=Debug\nMaxTraceSizeMB=0\n";
        write_text(ini, contents);
        const auto result = fc::validation::validate_offline(input);
        REQUIRE(result);
        CHECK(result->runtime_settings.trace_file_bytes == 0);
        CHECK(result->runtime_settings.log_level == FC_LOG_DEBUG);
        CHECK(fc::tests::read_text(ini) == contents);
        CHECK_FALSE(std::filesystem::exists(config / "FC.Core.ini"));
    }

    SECTION("A malformed runtime INI is an input error") {
        constexpr std::string_view contents = "[FusionCutter\nLogLevel=Debug\n";
        write_text(ini, contents);
        const auto result = fc::validation::validate_offline(input);
        REQUIRE_FALSE(result);
        CHECK(result.error().message.find("FusionCutter.ini") != std::string::npos);
        CHECK(fc::tests::read_text(ini) == contents);
    }
}

TEST_CASE("Offline validation recognizes renamed files and preserves their reported basenames", "[verifier][offline]") {
    fc::tests::TemporaryDirectory directory;
    const auto original = fc::tests::verifier::write_image(directory.path);
    const auto renamed = directory.path / "renamed-game.bin";
    std::filesystem::rename(original, renamed);
    fc::validation::ValidationInput input;
    input.images.push_back({renamed});
    input.plugins.push_back(FC_VERIFIER_FIXTURE);

    SECTION("PE facts identify the file without an explicit profile") {}
    SECTION("an explicit profile asserts the recognized build") {
        const auto catalog = fc::runtime::known_image_profiles();
        for (const auto &profile : catalog) {
            if (profile.id.starts_with("BF2.")) {
                input.images[0].profile = profile.id;
                break;
            }
        }
    }
    SECTION("a different requested profile rejects the renamed file") {
        const auto catalog = fc::runtime::known_image_profiles();
        input.images[0].profile = catalog.front().id;
        const auto result = fc::validation::validate_offline(input);
        REQUIRE_FALSE(result);
        CHECK(result.error().message.find("recognized image profile does not match the requested profile") != std::string::npos);
        return;
    }

    const auto result = fc::validation::validate_offline(input);

    REQUIRE(result);
    REQUIRE(result->images.size() == 1);
    CHECK(result->images[0].image.filename == "renamed-game.bin");
    const auto *ready = fc::validation::find_patch(*result, "VerifyFixture.Ready");
    REQUIRE(ready);
    CHECK(ready->state == fc::runtime::PatchState::Planning);
    REQUIRE(ready->plan);
    CHECK(ready->plan->operations().size() == 3);
}
