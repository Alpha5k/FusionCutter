#include "image_fixture.hpp"
#include "../test_support.hpp"

#include <catch2/catch_test_macros.hpp>
#include <Windows.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>

namespace {

// Quotes a test-controlled path or argument for a Windows child command line.
std::wstring quote(std::wstring_view text) {
    return L"\"" + std::wstring{text} + L"\"";
}

// Supplies the production profile matching the structural PE fixture.
std::wstring profile() {
    for (const auto &image : fc::runtime::known_image_profiles()) {
        if (image.id.starts_with("BF2.")) {
            return {image.id.begin(), image.id.end()};
        }
    }
    FAIL("The production catalog has no BF2 profile");
}

// Supplies only the fixture's explicit configuration; bundled patches retain their declaration defaults.
void configure(const std::filesystem::path &directory, std::string_view fixture = {}) {
    std::ofstream{directory / "FC.VerifyFixture.ini"} << fixture;
}

// Runs the actual verifier over the fixture DLL and file, optionally asserting one named patch.
DWORD verify(const std::filesystem::path &directory, const std::filesystem::path &image, const std::filesystem::path &report,
             std::wstring_view check = {}, std::wstring_view extra = {}) {
    auto command = quote(std::filesystem::path{FC_VERIFIER_EXECUTABLE}.wstring()) + L" --image " +
                   quote(profile() + L"=" + image.wstring()) + L" --plugin " + quote(std::filesystem::path{FC_VERIFIER_FIXTURE}.wstring()) +
                   L" --config " + quote(directory.wstring()) + L" --json " + quote(report.wstring());
    if (!check.empty()) {
        command += L" --check " + quote(check);
    }
    if (!extra.empty()) {
        command += L" " + std::wstring{extra};
    }
    return fc::tests::run_child(std::move(command), directory.c_str());
}

// Parses the report and checks the requested patch result and optional image path.
void check_json(const std::filesystem::path &directory, const std::filesystem::path &report, DWORD exit_code, std::wstring_view owner = {},
                std::wstring_view status = {}, const std::filesystem::path &image = {}) {
    auto command = quote(std::filesystem::path{FC_CMAKE_EXECUTABLE}.wstring()) + L" " + quote(L"-DFC_REPORT=" + report.wstring()) +
                   L" -DFC_EXIT_CODE=" + std::to_wstring(exit_code);
    if (!owner.empty()) {
        command += L" " + quote(L"-DFC_OWNER=" + std::wstring{owner}) + L" -DFC_STATUS=" + std::wstring{status};
    }
    if (!image.empty()) {
        command += L" " + quote(L"-DFC_IMAGE_PATH=" + image.wstring());
    }
    command += L" -P " + quote(std::filesystem::path{FC_VERIFIER_JSON_CHECK}.wstring());
    CHECK(fc::tests::run_child(std::move(command), directory.c_str()) == 0);
}
} // namespace

TEST_CASE("Verifier loads a real DLL and emits parsed results while preserving its input files", "[verifier]") {
    fc::tests::TemporaryDirectory directory;
    const auto original = fc::tests::verifier::write_image(directory.path);
    const auto image = directory.path / L"image-\u00e9.bin";
    std::filesystem::rename(original, image);
    configure(directory.path, "[Ready]\nvalue=19\n");
    const auto bytes = fc::tests::read_text(image);
    const auto settings = fc::tests::read_text(directory.path / "FC.VerifyFixture.ini");
    fc::tests::TemporaryDirectory output;
    const auto report = output.path / "result.json";
    // NOP-only Steam/GOG images fail bundled Core Lua evidence while the independent Ready fixture still passes.
    const DWORD expected = profile().starts_with(L"BF2.Classic.") ? 0 : 1;
    CHECK(verify(directory.path, image, report, L"VerifyFixture.Ready") == expected);
    check_json(directory.path, report, expected, L"VerifyFixture.Ready", L"passed", image);
    CHECK(fc::tests::read_text(image) == bytes);
    CHECK(fc::tests::read_text(directory.path / "FC.VerifyFixture.ini") == settings);
    CHECK_FALSE(std::filesystem::exists(directory.path / "FusionCutter.ini"));

    SECTION("A report cannot overwrite an image through another filename") {
        const auto alias = directory.path / "image-alias.bin";
        std::filesystem::create_hard_link(image, alias);
        CHECK(verify(directory.path, image, alias) == 2);
        CHECK(fc::tests::read_text(image) == bytes);
    }
}

TEST_CASE("Named verifier checks reject patches that cannot complete offline validation", "[verifier]") {
    fc::tests::TemporaryDirectory directory;
    const auto image = fc::tests::verifier::write_image(directory.path);
    configure(directory.path, "[Fail]\nenabled=true\n");
    fc::tests::TemporaryDirectory output;
    const auto report = output.path / "result.json";
    CHECK(verify(directory.path, image, report, L"VerifyFixture.Fail", L"--check Missing.Patch") == 1);
    check_json(directory.path, report, 1, L"VerifyFixture.Fail", L"failed");
    check_json(directory.path, report, 1, L"Missing.Patch", L"failed");
}

TEST_CASE("Verifier reports unusable input paths separately from rejected plugins", "[verifier]") {
    fc::tests::TemporaryDirectory directory;
    fc::tests::TemporaryDirectory output;
    const auto image = fc::tests::verifier::write_image(directory.path);
    configure(directory.path);
    const auto report = output.path / "result.json";
    CHECK(verify(directory.path, image, report, {}, L"--plugin missing.dll") == 2);
    check_json(directory.path, report, 2);

    const auto rejected = directory.path / "rejected.dll";
    std::ofstream{rejected} << "This is not a DLL.\n";
    CHECK(verify(directory.path, image, report, {}, L"--plugin " + quote(rejected.wstring())) == 1);
    check_json(directory.path, report, 1);
}
