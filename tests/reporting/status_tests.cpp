#include "reporting/status.hpp"
#include "reporting/reporting.hpp"
#include "../test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <Windows.h>

#include <filesystem>
#include <string>

TEST_CASE("Status publishing preserves unchanged reports and retries failed writes") {
    fc::tests::TemporaryDirectory directory;
    const auto path = directory.path / "FusionCutter.txt";
    fc::reporting::StatusPublisher publisher{path};
    REQUIRE(publisher.publish("Installed: Example\n").outcome == fc::reporting::StatusPublishOutcome::Written);
    CHECK(fc::tests::read_text(path).find("Installed: Example") != std::string::npos);
    CHECK(publisher.publish("Installed: Example\n").outcome == fc::reporting::StatusPublishOutcome::Unchanged);

    // Failed content remains eligible for publication when the file obstruction is removed.
    REQUIRE(std::filesystem::remove(path));
    REQUIRE(std::filesystem::create_directory(path));
    CHECK(publisher.publish("Waiting\n").outcome == fc::reporting::StatusPublishOutcome::Failed);
    REQUIRE(std::filesystem::remove(path));
    REQUIRE(publisher.publish("Waiting\n").outcome == fc::reporting::StatusPublishOutcome::Written);
    const auto report = fc::tests::read_text(path);
    CHECK(report.find("Waiting") != std::string::npos);
}

TEST_CASE("The reporting worker retries failed status writes with the latest report") {
    fc::tests::TemporaryDirectory directory;
    const auto path = directory.path / "FusionCutter.txt";
    REQUIRE(std::filesystem::create_directory(path));
    fc::reporting::ReportingSession session;
    session.start({}, directory.path);
    session.configure(FC_LOG_OFF);
    session.publish_status("Old pending snapshot\n");
    session.publish_status("Latest pending snapshot\n");
    Sleep(100);
    REQUIRE(std::filesystem::remove(path));
    for (unsigned retry = 0; retry != 500; ++retry) {
        if (fc::tests::read_text(path).find("Latest pending snapshot") != std::string::npos) {
            break;
        }
        Sleep(10);
    }
    CHECK(fc::tests::read_text(path).find("Latest pending snapshot") != std::string::npos);
}
