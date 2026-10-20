#include "core_runtime.hpp"
#include "reporting/reporting.hpp"
#include "reporting_test_support.hpp"

#include <fc/reporting.hpp>

#include <catch2/catch_test_macros.hpp>

#include <Windows.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using fc::tests::reporting::entered;
using fc::tests::reporting::ReleaseWorker;
using fc::tests::reporting::resume;
using fc::tests::reporting::WorkerGate;

// Keeps the log worker paused until the DLL has released its formatting arguments.
std::uint32_t FC_CDECL pause_formatter(FC_Bytes, FC_TextSink) {
    SetEvent(entered);
    WaitForSingleObject(resume, INFINITE);
    return 1;
}
} // namespace

TEST_CASE("Log filtering and formatting preserve accepted messages") {
    fc::tests::TemporaryDirectory directory;
    {
        fc::runtime::CoreRuntime runtime{{}};
        runtime.reporting().start({}, directory.path);
        const fc::Logger log{{nullptr, &runtime.host()}};
        CHECK_FALSE(log.info("Filtered info"));
        REQUIRE(log.warning("Values: {} {} {:.1f}", 42, true, 2.5));
        REQUIRE(log.error("Provider reason: {}", std::string_view{"First\nSecond\tThird"}));
        REQUIRE(log.write(fc::LogLevel::Error, "Raw message"));
        runtime.reporting().configure(FC_LOG_OFF);
        CHECK_FALSE(log.error("Disabled error"));
    }

    const auto log = fc::tests::read_text(directory.path / "FusionCutter.log");
    CHECK(log.find("[Warning] [Fusion Cutter] Values: 42 true 2.5") != std::string::npos);
    CHECK(log.find("Provider reason: First Second Third") != std::string::npos);
    CHECK(log.find("Raw message") != std::string::npos);
    CHECK(log.find("Filtered info") == std::string::npos);
    CHECK(log.find("Disabled error") == std::string::npos);
}

TEST_CASE("Log rotation keeps two backups and writes the complete next record") {
    fc::tests::TemporaryDirectory directory;
    const auto path = directory.path / "FusionCutter.log";
    {
        std::ofstream current{path, std::ios::binary};
        current << "Previous current log";
        std::ofstream first{directory.path / "FusionCutter.1.log"};
        first << "Previous first backup";
    }
    // The next complete record must rotate a file that has only eight bytes left.
    std::filesystem::resize_file(path, 4U * 1024U * 1024U - 8);
    {
        fc::reporting::ReportingSession session;
        session.start({}, directory.path);
        REQUIRE(session.write(nullptr, FC_LOG_WARNING, {"Complete rotated record", 23}));
    }

    CHECK(fc::tests::read_text(path).find("Complete rotated record") != std::string::npos);
    CHECK(fc::tests::read_text(directory.path / "FusionCutter.1.log").starts_with("Previous current log"));
    CHECK(fc::tests::read_text(directory.path / "FusionCutter.2.log") == "Previous first backup");
}

TEST_CASE("Log output reports a file failure and accepts messages after recovery") {
    fc::tests::TemporaryDirectory directory;
    const auto path = directory.path / "FusionCutter.log";
    // A directory prevents the worker from opening the log file.
    REQUIRE(std::filesystem::create_directory(path));
    {
        fc::reporting::ReportingSession session;
        session.start({}, directory.path);
        REQUIRE(session.write(nullptr, FC_LOG_WARNING, {"Initial output", 14}));
        for (unsigned retry = 0; retry != 500 && !session.status().output_failed; ++retry) {
            Sleep(10);
        }
        REQUIRE(session.status().output_failed);
        CHECK_FALSE(session.write(nullptr, FC_LOG_WARNING, {"Refused output", 14}));

        REQUIRE(std::filesystem::remove(path));
        for (unsigned retry = 0; retry != 500 && session.status().output_failed; ++retry) {
            Sleep(10);
        }
        REQUIRE_FALSE(session.status().output_failed);
        REQUIRE(session.write(nullptr, FC_LOG_WARNING, {"Recovered output", 16}));
    }
    CHECK(fc::tests::read_text(path).find("Recovered output") != std::string::npos);
}

TEST_CASE("An SDK plugin preserves log arguments and trace payloads after their source buffers change") {
    fc::tests::TemporaryDirectory directory;
    const auto producer = GetCurrentThreadId();
    const std::array paths{std::filesystem::path{FC_REPORTING_FIXTURE}};
    auto admitted = fc::runtime::load_plugins(paths);
    REQUIRE(admitted.diagnostics.empty());
    REQUIRE(admitted.admitted.size() == 1);

    // The admitted DLL, patch attribution and events outlive both runtime workers.
    fc::runtime::PluginRecord plugin;
    plugin.admitted = admitted.admitted.front();
    fc::runtime::PatchRecord owner;
    owner.plugin = &plugin;
    owner.definition = &plugin.admitted.definition->patches[0];
    WorkerGate gate;
    std::filesystem::path trace_file;
    {
        fc::runtime::CoreRuntime runtime{{}};
        ReleaseWorker resume_worker;
        owner.runtime = &runtime;
        owner.state = fc::runtime::PatchState::Preparing;
        runtime.reporting().start({}, directory.path);
        runtime.traces().configure(8U * 1024U * 1024U, directory.path, &runtime.reporting());
        REQUIRE(runtime.reporting().submit(nullptr, FC_LOG_WARNING, {{nullptr, 0}, &pause_formatter}));
        REQUIRE(WaitForSingleObject(entered, 5000) == WAIT_OBJECT_0);

        FC_Handler handler{};
        const auto &factory = owner.definition->variants[0].factory;
        REQUIRE(factory.create(factory.context, &owner, &runtime.host(), nullptr, 0, &handler, {}) == 1);
        REQUIRE(handler.context);
        REQUIRE(handler.prepare);
        const auto accepted = handler.prepare(handler.context, {});
        // Formatting must depend only on copied arguments after the handler is destroyed.
        handler.destroy(handler.context);
        REQUIRE(accepted == 1);
        SetEvent(resume);
        runtime.finish_reporting();
        REQUIRE(runtime.traces().status().path);
        trace_file = *runtime.traces().status().path;
    }

    const auto log = fc::tests::read_text(directory.path / "FusionCutter.log");
    CHECK(log.find("DLL inputs: 42 true 2.5 Temporary DLL text Temporary DLL text") != std::string::npos);
    CHECK(log.find("[ReportingFixture/Output]") != std::string::npos);
    CHECK(log.find("DLL preformatted text") != std::string::npos);

    const auto decoded = fc::tests::reporting::read_trace(trace_file);
    REQUIRE(decoded.records.size() == 2);
    CHECK(decoded.records[0].version == 5);
    CHECK(decoded.records[0].thread == producer);
    CHECK(decoded.records[0].timestamp != 0);
    CHECK(decoded.records[0].payload == std::vector<std::uint8_t>{0x78, 0x56, 0x34, 0x12});
    CHECK(decoded.records[1].payload == std::vector<std::uint8_t>{1, 2, 3});
    CHECK(decoded.records[0].sequence == 0);
    CHECK(decoded.records[1].sequence == 1);
}
