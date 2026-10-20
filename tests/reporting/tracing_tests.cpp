#include "reporting/tracing.hpp"
#include "core_runtime.hpp"
#include "reporting/reporting.hpp"
#include "reporting_test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <Windows.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

TEST_CASE("Explicit runtime reporting shutdown preserves accepted logs and finalizes traces") {
    fc::tests::TemporaryDirectory directory;
    fc::runtime::CoreRuntime runtime{{}};
    auto &reporting = runtime.reporting();
    reporting.start({}, directory.path);
    auto &traces = runtime.traces();
    traces.configure(8U * 1024U * 1024U, directory.path, &reporting);
    auto channel = traces.create_channel(nullptr, {{"Owned", 5}, 1, 65536});
    REQUIRE(channel);
    const std::array<std::uint8_t, 4> payload{0x21, 0x43, 0x65, 0x87};
    REQUIRE((*channel)->write({payload.data(), static_cast<std::uint32_t>(payload.size())}));
    REQUIRE(reporting.write(nullptr, FC_LOG_WARNING, {"Last admitted log", 17}));

    // Explicit shutdown must close ETW while the runtime and channel handles are still alive.
    runtime.finish_reporting();
    runtime.finish_reporting();
    CHECK_FALSE((*channel)->enabled());
    CHECK_FALSE((*channel)->write({nullptr, 0}));
    CHECK_FALSE(traces.create_channel(nullptr, {{"Late", 4}, 1, 1024}));
    CHECK_FALSE(reporting.write(nullptr, FC_LOG_WARNING, {"Late log", 8}));
    CHECK(fc::tests::read_text(directory.path / "FusionCutter.log").find("Last admitted log") != std::string::npos);
    REQUIRE(traces.status().path);
    const auto decoded = fc::tests::reporting::read_trace(*traces.status().path);
    REQUIRE(decoded.records.size() == 1);
    CHECK(decoded.records.front().payload == std::vector<std::uint8_t>{payload.begin(), payload.end()});
}

TEST_CASE("Disabled trace channels refuse records and create no output") {
    fc::tests::TemporaryDirectory directory;
    fc::runtime::TraceSession traces;
    traces.configure(0, directory.path);
    auto channel = traces.create_channel(nullptr, {{"Disabled", 8}, 1, 1024});
    REQUIRE(channel);
    CHECK(traces.status().configured_disabled);
    CHECK_FALSE((*channel)->enabled());
    CHECK_FALSE((*channel)->write({nullptr, 0}));
    traces.shutdown();
    CHECK_FALSE(std::filesystem::exists(directory.path / "traces"));
}

TEST_CASE("Trace setup failure is reported through channel health") {
    fc::tests::TemporaryDirectory directory;
    {
        // A file at the directory path makes the first trace output attempt fail.
        std::ofstream blocked{directory.path / "traces"};
        blocked << "This path is a file";
    }
    fc::runtime::TraceSession traces;
    traces.configure(8U * 1024U * 1024U, directory.path);
    auto channel = traces.create_channel(nullptr, {{"Failure", 7}, 1, 1024});
    REQUIRE(channel);
    REQUIRE((*channel)->write({nullptr, 0}));
    traces.shutdown();
    CHECK((*channel)->health().output_failed == 1);
    CHECK_FALSE(traces.status().path);
}
