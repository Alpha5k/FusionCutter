#include "../test_support.hpp"
#include <catch2/catch_test_macros.hpp>
#include <Windows.h>
#include <filesystem>
#include <string>
#include <string_view>

namespace {

// Runs the real runtime and selected plugin DLLs in a fresh process with a private configuration directory.
DWORD run_child(std::string_view mode, const std::filesystem::path &directory) {
    const auto child = directory / "fc_control_child.exe";
    std::filesystem::copy_file(FC_CONTROL_CHILD, child);
    std::filesystem::create_directory(directory / "plugins");
    std::filesystem::copy_file(FC_API_PROVIDER, directory / "plugins" / "provider.dll");
    std::filesystem::copy_file(FC_API_CONSUMER, directory / "plugins" / "consumer.dll");

    auto command = L"\"" + child.wstring() + L"\" " + std::wstring{mode.begin(), mode.end()};
    return fc::tests::run_child(std::move(command), directory.c_str());
}
} // namespace

TEST_CASE("Separate DLLs connect APIs and retain an optional callback capture", "[runtime][patch-api]") {
    fc::tests::TemporaryDirectory directory;
    CHECK(run_child("valid", directory.path) == 0);
    const auto status = fc::tests::read_text(directory.path / "FusionCutter.txt");
    CHECK(status.find("DLL API value=42 available=15") != std::string::npos);
    CHECK(status.find("ApiProvider 1: 1 installed") != std::string::npos);
    CHECK(status.find("ApiProvider.Publish\n") == std::string::npos);
}

TEST_CASE("Equal-size DLL API signature and record mismatches fail only the consumer", "[runtime][patch-api]") {
    for (const auto mode : {"signature", "layout"}) {
        fc::tests::TemporaryDirectory directory;
        INFO(mode);
        CHECK(run_child(mode, directory.path) == 0);
        const auto status = fc::tests::read_text(directory.path / "FusionCutter.txt");
        CHECK(status.find("ApiConsumer.Read: Failed") != std::string::npos);
        CHECK(status.find("ApiProvider 1: 1 installed") != std::string::npos);
    }
}

TEST_CASE("Consumer faults terminate the process before the provider's enclosing exception handler can catch them", "[runtime][fatal]") {
    fc::tests::TemporaryDirectory directory;
    // Unicode DLL paths must retain their native module facts when runtime startup publishes crash annotations.
    auto unicode = directory.path;
    unicode += L"-\u00e9\u6f22";
    std::filesystem::rename(directory.path, unicode);
    directory.path = unicode;

    CHECK(run_child("consumer-fault", directory.path) == 0xd1);

    const auto crash = fc::tests::read_text(directory.path / "FusionCutter.crash.log");
    CHECK(crash.find("Patch callback raised a native fault") != std::string::npos);
    CHECK(crash.find("ApiConsumer.Read") != std::string::npos);
    CHECK(crash.find("ApiProvider") != std::string::npos);
    CHECK(crash.find("ApiConsumer") != std::string::npos);

    // These fields on the consumer's inventory line require the production native module lookup to succeed.
    const auto plugin = crash.find("  Plugin ApiConsumer 1 module=");
    REQUIRE(plugin != std::string::npos);
    const auto remaining = std::string_view{crash}.substr(plugin);
    const auto annotation = remaining.substr(0, remaining.find('\n'));
    CHECK(annotation.find("\xc3\xa9\xe6\xbc\xa2") != std::string_view::npos);
    CHECK(annotation.find(" base=") != std::string_view::npos);
    CHECK(annotation.find(" size=") != std::string_view::npos);
    CHECK(annotation.find(" timestamp=") != std::string_view::npos);
}

TEST_CASE("A crash report identifies a DLL loaded after runtime startup by profile, offset and timestamp", "[runtime][reporting]") {
    fc::tests::TemporaryDirectory directory;
    CHECK(run_child("image-late", directory.path) == 0);
}

TEST_CASE("A waiting patch requires every image and keeps captured settings while independent updates continue", "[runtime][lifecycle]") {
    fc::tests::TemporaryDirectory directory;
    CHECK(run_child("late-settings", directory.path) == 0);
}

TEST_CASE("An incompatible late DLL fails its waiting patch while independent updates continue", "[runtime][lifecycle]") {
    fc::tests::TemporaryDirectory directory;
    CHECK(run_child("image-incompatible", directory.path) == 0);
}

TEST_CASE("An explicit fatal error preserves its reason and consumer identity in the crash report", "[runtime][fatal]") {
    fc::tests::TemporaryDirectory directory;
    CHECK(run_child("explicit-fault", directory.path) == 0xd1);

    const auto crash = fc::tests::read_text(directory.path / "FusionCutter.crash.log");
    CHECK(crash.find("Deliberate consumer fatal") != std::string::npos);
    CHECK(crash.find("ApiConsumer.Read") != std::string::npos);
}

TEST_CASE("Late preparation finishes before the next installed update time is sampled", "[runtime][lifecycle]") {
    fc::tests::TemporaryDirectory directory;
    CHECK(run_child("timing-late", directory.path) == 0);
}
