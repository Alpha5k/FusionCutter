#include "catalog/definition_copy.hpp"
#include "reporting/reporting.hpp"
#include "reporting/crash_reporter.hpp"
#include "reporting/tracing.hpp"
#include "runtime/patch_runtime.hpp"

#include <FusionCutter/CoreApi.h>

#include <catch2/catch_test_macros.hpp>

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

// Each test owns a unique support directory so deferred file creation cannot observe output from another session.
class TemporaryDirectory final {
  public:
    TemporaryDirectory() {
        static std::atomic_uint64_t sequence{};
        path = std::filesystem::temp_directory_path() /
               ("FusionCutter-Reporting-" + std::to_string(GetCurrentProcessId()) + "-" +
                std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)));
        REQUIRE(std::filesystem::create_directory(path));
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }

    std::filesystem::path path;
};

[[nodiscard]] std::string read_text(const std::filesystem::path& path) {
    std::ifstream input{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

// The helper contains SEH in a function with no C++ unwind state, allowing the VEH to observe then continue search.
void raise_caught(DWORD code) noexcept {
    __try {
        RaiseException(code, 0, 0, nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // The test owns the terminal handler only so the process can inspect FusionCutter.Crash.log afterward.
    }
}

// The native status fixture submits malformed fields and can inject a callback failure after partial output.
struct StatusFixture {
    bool fail_callback{};
    std::size_t callback_count{};
};

void FC_CALL destroy_status_patch(void*, FC_PatchHandle) noexcept {}

void FC_CALL write_status(void*, FC_PatchHandle patch, const FC_StatusSink* sink) {
    auto& fixture = *static_cast<StatusFixture*>(patch);
    ++fixture.callback_count;
    constexpr std::string_view valid_label = "Healthy";
    constexpr std::string_view invalid_label = "Rejected";
    static_cast<void>(sink->add_boolean(sink->context,
                                        {valid_label.data(), static_cast<std::uint32_t>(valid_label.size())}, FC_TRUE));
    static_cast<void>(
        sink->add_boolean(sink->context, {invalid_label.data(), static_cast<std::uint32_t>(invalid_label.size())}, 7));
    static_cast<void>(sink->add_text(sink->context, {nullptr, 1}, {}));
    if (fixture.fail_callback) {
        throw std::runtime_error{"injected live status failure"};
    }
}

// A minimal coherent target gives StatusPublisher the same process-lifetime runtime owner used in production.
[[nodiscard]] fc::targets::RecognizedTarget status_target() {
    std::vector<fc::targets::OwnedImage> images;
#if defined(_M_X64)
    auto bootstrap = std::vector<std::byte>(1);
    images.push_back(fc::targets::OwnedImage::mapped(
        {FC_IMAGE_BOOTSTRAP, "ClassicCollection_Bootstrap_66702CD7", 0, bootstrap.size()}, std::move(bootstrap), {}));
    constexpr std::string_view game_profile = "ClassicCollection_Game_66702CD2";
#else
    constexpr std::string_view game_profile = "SteamRetail_Game_59EDE353";
#endif
    auto game = std::vector<std::byte>(1);
    images.push_back(
        fc::targets::OwnedImage::mapped({FC_IMAGE_GAME, game_profile, 0, game.size()}, std::move(game), {}));
    auto target = fc::targets::RecognizedTarget::create(FC_HOST_ROLE_CLIENT, std::move(images));
    REQUIRE(target.has_value());
    return std::move(*target);
}

} // namespace

TEST_CASE("status remains eager while a quiet session creates no log") {
    TemporaryDirectory directory;
    fc::reporting::ReportingSession reporting;
    reporting.start({.started = std::chrono::system_clock::now(),
                     .role = FC_HOST_ROLE_CLIENT,
                     .loader_kind = FC_LOADER_KIND_DINPUT8,
                     .direct_input_chain = FC_DIRECT_INPUT_CHAIN_LOADED,
                     .selected_proxy_basename = "proxy\nname.dll"},
                    directory.path);
    fc::runtime::TraceSession traces{0};

    reporting.publish(fc::reporting::InitializationStatus::Completed, nullptr, nullptr, traces, true);

    REQUIRE(std::filesystem::exists(directory.path / "FusionCutter.txt"));
    CHECK_FALSE(std::filesystem::exists(directory.path / "FusionCutter.log"));
    const auto status = read_text(directory.path / "FusionCutter.txt");
    CHECK(status.find("Initialization: Completed") != std::string::npos);
    CHECK(status.find("timestamp=0x") != std::string::npos);
    CHECK(status.find("proxy name.dll") != std::string::npos);
}

TEST_CASE("status suppresses unchanged replacements and retries a failed changed snapshot") {
    TemporaryDirectory directory;
    const auto path = directory.path / "FusionCutter.txt";
    fc::reporting::StatusPublisher status{path};
    status.set_session({.started = std::chrono::system_clock::now(), .role = FC_HOST_ROLE_CLIENT});
    const fc::reporting::LogStatus log{.level = FC_LOG_OFF};
    const fc::reporting::TraceStatus trace{};

    REQUIRE(status.publish(fc::reporting::InitializationStatus::Completed, nullptr, nullptr, {}, log, trace, true) ==
            fc::reporting::StatusPublishResult::Written);
    const auto first_write = std::filesystem::last_write_time(path);
    CHECK(status.publish(fc::reporting::InitializationStatus::Completed, nullptr, nullptr, {}, log, trace, true) ==
          fc::reporting::StatusPublishResult::Unchanged);
    CHECK(std::filesystem::last_write_time(path) == first_write);

    // A directory at the final filename forces direct replacement to fail without creating a temporary-file path.
    REQUIRE(std::filesystem::remove(path));
    REQUIRE(std::filesystem::create_directory(path));
    CHECK(status.publish(fc::reporting::InitializationStatus::Fatal, nullptr, nullptr, {}, log, trace, true) ==
          fc::reporting::StatusPublishResult::Failed);
    REQUIRE(std::filesystem::remove(path));
    CHECK(status.publish(fc::reporting::InitializationStatus::Fatal, nullptr, nullptr, {}, log, trace, true) ==
          fc::reporting::StatusPublishResult::Written);
    CHECK(read_text(path).find("Initialization: Fatal") != std::string::npos);
}

TEST_CASE("every rejected live status addition contributes to the bounded omission count") {
    TemporaryDirectory directory;
    fc::catalog::PluginRecord plugin;
    plugin.definition.id = "StatusFixture";
    plugin.definition.patches.push_back({.id = "StatusPatch", .name = "Status patch"});
    std::vector<fc::catalog::PluginRecord> plugins;
    plugins.push_back(std::move(plugin));
    fc::runtime::PatchRuntimeState runtime{status_target(), fc::catalog::Catalog{std::move(plugins)}};
    StatusFixture fixture;
    static const FC_PatchCallbacks callbacks{
        .destroy = &destroy_status_patch,
        .write_status = &write_status,
    };
    runtime.installed_patches.push_back(
        {fc::catalog::PatchIndex{0}, {}, fc::planning::PatchInstance{callbacks, &fixture}});
    fc::reporting::StatusPublisher status{directory.path / "FusionCutter.txt"};

    const auto live = status.collect_live(runtime);
    REQUIRE(live.size() == 1);
    REQUIRE(live.front().fields.size() == 1);
    CHECK(live.front().fields.front().label == "Healthy");
    CHECK(live.front().omitted == 2);
}

TEST_CASE("a caught live status callback failure remains visible to the reporting owner") {
    TemporaryDirectory directory;
    fc::catalog::PluginRecord plugin;
    plugin.definition.id = "StatusFixture";
    plugin.definition.patches.push_back({.id = "StatusPatch", .name = "Status patch"});
    std::vector<fc::catalog::PluginRecord> plugins;
    plugins.push_back(std::move(plugin));
    fc::runtime::PatchRuntimeState runtime{status_target(), fc::catalog::Catalog{std::move(plugins)}};
    StatusFixture fixture{.fail_callback = true};
    static const FC_PatchCallbacks callbacks{
        .destroy = &destroy_status_patch,
        .write_status = &write_status,
    };
    runtime.installed_patches.push_back(
        {fc::catalog::PatchIndex{0}, {}, fc::planning::PatchInstance{callbacks, &fixture}});
    fc::reporting::StatusPublisher status{directory.path / "FusionCutter.txt"};

    const auto live = status.collect_live(runtime);

    REQUIRE(live.size() == 1);
    CHECK(live.front().callback_failed);
    CHECK(live.front().omitted == 3);

    // Repeated failed snapshots produce one transition warning instead of flooding the periodic reporting cadence.
    fc::reporting::ReportingSession reporting;
    reporting.start({.started = std::chrono::system_clock::now(), .role = FC_HOST_ROLE_CLIENT}, directory.path);
    reporting.configure(FC_LOG_DEBUG);
    reporting.set_catalog(runtime.catalog, {});
    fc::runtime::TraceSession traces{0};
    reporting.publish(fc::reporting::InitializationStatus::Completed, nullptr, &runtime, traces, true);
    reporting.publish(fc::reporting::InitializationStatus::Completed, nullptr, &runtime, traces, true);
    reporting.fatal("Flush the callback diagnostic", &runtime, traces);
    const auto log = read_text(directory.path / "FusionCutter.log");
    const auto warning = log.find("live status callback failed");
    REQUIRE(warning != std::string::npos);
    CHECK(log.find("live status callback failed", warning + 1) == std::string::npos);
}

TEST_CASE("patch status callbacks follow the one-second publication cadence") {
    TemporaryDirectory directory;
    fc::catalog::PluginRecord plugin;
    plugin.definition.id = "StatusFixture";
    plugin.definition.patches.push_back({.id = "StatusPatch", .name = "Status patch"});
    std::vector<fc::catalog::PluginRecord> plugins;
    plugins.push_back(std::move(plugin));
    fc::runtime::PatchRuntimeState runtime{status_target(), fc::catalog::Catalog{std::move(plugins)}};
    StatusFixture fixture;
    static const FC_PatchCallbacks callbacks{
        .destroy = &destroy_status_patch,
        .write_status = &write_status,
    };
    runtime.installed_patches.push_back(
        {fc::catalog::PatchIndex{0}, {}, fc::planning::PatchInstance{callbacks, &fixture}});

    fc::reporting::ReportingSession reporting;
    reporting.start({.started = std::chrono::system_clock::now(), .role = FC_HOST_ROLE_CLIENT}, directory.path);
    reporting.set_catalog(runtime.catalog, {});
    fc::runtime::TraceSession traces{0};
    reporting.publish(fc::reporting::InitializationStatus::Completed, nullptr, &runtime, traces, true);
    reporting.publish(fc::reporting::InitializationStatus::Completed, nullptr, &runtime, traces, false);

    CHECK(fixture.callback_count == 1);
}

TEST_CASE("accepted scoped logs lazily create the file chosen by rotation policy with session context") {
    TemporaryDirectory directory;
    fc::reporting::ReportingSession reporting;
    reporting.start({.started = std::chrono::system_clock::now(), .role = FC_HOST_ROLE_SERVER}, directory.path);
    reporting.configure(FC_LOG_DEBUG);
    fc::catalog::PluginRecord core_plugin;
    core_plugin.definition.id = "Core";
    core_plugin.definition.patches.push_back({.id = "LoggingProbe"});
    std::vector<fc::catalog::PluginRecord> plugins;
    plugins.push_back(std::move(core_plugin));
    const fc::catalog::Catalog catalog{std::move(plugins)};
    reporting.set_catalog(catalog, {});
    const std::string message = "first line\nsecond line";
    reporting.write(nullptr, FC_LOG_INFO, {message.data(), static_cast<std::uint32_t>(message.size())});
    // Framework components use the same queue and file while retaining a precise subsystem source.
    reporting.logger("Validation").debug("Validated {} patch", "fixture");
    // A built-in plugin patch retains ordinary plugin attribution and therefore cannot impersonate framework scopes.
    constexpr std::string_view plugin_message = "Built-in plugin diagnostic";
    reporting.write(fc::catalog::report_token({0}), FC_LOG_INFO,
                    {plugin_message.data(), static_cast<std::uint32_t>(plugin_message.size())});
    fc::runtime::TraceSession traces{0};

    // Publishing a Fatal result performs the synchronous startup flush used before loader termination.
    const auto flush_started = std::chrono::steady_clock::now();
    reporting.fatal("A test fatal boundary was reached", nullptr, traces);
    const auto flush_elapsed = std::chrono::steady_clock::now() - flush_started;

    REQUIRE(std::filesystem::exists(directory.path / "FusionCutter.log"));
    const auto log = read_text(directory.path / "FusionCutter.log");
    CHECK(log.find("Fusion Cutter") != std::string::npos);
    CHECK(log.find("Role: Server") != std::string::npos);
    CHECK(log.find("[FusionCutter] first line\nsecond line") != std::string::npos);
    CHECK(log.find("[FusionCutter/Validation] Validated fixture patch") != std::string::npos);
    CHECK(log.find("[Core/LoggingProbe] Built-in plugin diagnostic") != std::string::npos);
    CHECK(log.find("[FusionCutter/Runtime] Fatal runtime invariant: A test fatal boundary was reached") !=
          std::string::npos);
    // The production budget is 250 ms; this generous ceiling catches an accidental return to an unbounded backend API.
    CHECK(flush_elapsed < std::chrono::seconds{2});
}

TEST_CASE("framework log filtering occurs before scoped records enter the shared backend") {
    TemporaryDirectory directory;
    fc::reporting::ReportingSession reporting;
    reporting.start({.started = std::chrono::system_clock::now(), .role = FC_HOST_ROLE_CLIENT}, directory.path);
    reporting.configure(FC_LOG_WARNING);
    const auto selection = reporting.logger("Selection");

    // The filtered record must consume neither file space nor aggregate queue accounting; the warning creates output.
    selection.debug("This filtered decision must not be rendered");
    selection.warning("Patch '{}' was skipped", "Example");
    fc::runtime::TraceSession traces{0};
    reporting.publish(fc::reporting::InitializationStatus::Fatal, nullptr, nullptr, traces, true);

    const auto log = read_text(directory.path / "FusionCutter.log");
    CHECK(log.find("This filtered decision") == std::string::npos);
    CHECK(log.find("[FusionCutter/Selection] Patch 'Example' was skipped") != std::string::npos);
}

TEST_CASE("status names the framework configuration path after the plugin catalog is published") {
    TemporaryDirectory directory;
    fc::reporting::ReportingSession reporting;
    reporting.start({.started = std::chrono::system_clock::now(), .role = FC_HOST_ROLE_CLIENT}, directory.path);
    // An empty plugin catalog is sufficient because this assertion targets reporting's configuration fact.
    const fc::catalog::Catalog catalog{std::vector<fc::catalog::PluginRecord>{}};
    reporting.set_catalog(catalog, {});
    fc::runtime::TraceSession traces{0};
    reporting.publish(fc::reporting::InitializationStatus::Completed, nullptr, nullptr, traces, true);

    const auto status = read_text(directory.path / "FusionCutter.txt");
    CHECK(status.find("Configuration: config\\FC.Core.ini") != std::string::npos);
}

TEST_CASE("one encoded log record beyond the aggregate byte budget is dropped before file creation") {
    TemporaryDirectory directory;
    fc::reporting::ReportingSession reporting;
    reporting.start({.started = std::chrono::system_clock::now(), .role = FC_HOST_ROLE_CLIENT}, directory.path);
    reporting.configure(FC_LOG_DEBUG);
    const std::string oversized(2U * 1024U * 1024U, 'x');

    reporting.write(nullptr, FC_LOG_INFO, {oversized.data(), static_cast<std::uint32_t>(oversized.size())});
    fc::runtime::TraceSession traces{0};
    reporting.publish(fc::reporting::InitializationStatus::Completed, nullptr, nullptr, traces, true);

    CHECK_FALSE(std::filesystem::exists(directory.path / "FusionCutter.log"));
    const auto status = read_text(directory.path / "FusionCutter.txt");
    CHECK(status.find("1 dropped") != std::string::npos);
}

TEST_CASE("log rotation begins only when the next accepted record would exceed four MiB") {
    TemporaryDirectory directory;
    fc::reporting::ReportingSession reporting;
    reporting.start({.started = std::chrono::system_clock::now(), .role = FC_HOST_ROLE_CLIENT}, directory.path);
    reporting.configure(FC_LOG_DEBUG);
    fc::runtime::TraceSession traces{0};
    const std::string payload(64U * 1024U, 'r');

    for (unsigned batch = 0; batch < 5; ++batch) {
        for (unsigned record = 0; record < 16; ++record) {
            reporting.write(nullptr, FC_LOG_INFO, {payload.data(), static_cast<std::uint32_t>(payload.size())});
        }
        // The fatal form supplies a bounded flush gate without changing the file policy exercised by the records.
        reporting.publish(fc::reporting::InitializationStatus::Fatal, nullptr, nullptr, traces, true);
    }

    CHECK(std::filesystem::exists(directory.path / "FusionCutter.log"));
    CHECK(std::filesystem::exists(directory.path / "FusionCutter.1.log"));
    CHECK_FALSE(std::filesystem::exists(directory.path / "FusionCutter.2.log"));
    CHECK(std::filesystem::file_size(directory.path / "FusionCutter.1.log") <= 4U * 1024U * 1024U);
}

TEST_CASE("trace channels remain inactive before arm and create ETL only after an accepted record") {
    TemporaryDirectory directory;
    fc::runtime::TraceSession traces;
    traces.configure(8, directory.path);
    const std::string name = "Packets";
    const FC_TraceDefinition definition{.struct_size = sizeof(FC_TraceDefinition),
                                        .name = {name.data(), static_cast<std::uint32_t>(name.size())},
                                        .capacity = 8,
                                        .max_record_size = 32,
                                        .version = 3};
    fc::runtime::PreparedTraceChannel preparation;
    FC_TraceHandle handle{};
    REQUIRE(traces.prepare_channel({4}, &definition, preparation, &handle) == FC_TRACE_CREATED);
    const std::array record{std::byte{1}, std::byte{2}, std::byte{3}};
    CHECK(traces.try_write(handle, {reinterpret_cast<const std::uint8_t*>(record.data()),
                                    static_cast<std::uint32_t>(record.size())}) == FC_FALSE);
    CHECK_FALSE(std::filesystem::exists(directory.path / "traces"));

    traces.arm(preparation);
    REQUIRE(traces.try_write(handle, {reinterpret_cast<const std::uint8_t*>(record.data()),
                                      static_cast<std::uint32_t>(record.size())}) == FC_TRUE);

    FC_TraceHealth health{.struct_size = sizeof(FC_TraceHealth)};
    for (unsigned attempt = 0; attempt < 200; ++attempt) {
        traces.health(handle, &health);
        if (health.written == 1 || health.output_failed == FC_TRUE) {
            break;
        }
        // The bounded poll waits only for the below-normal writer; producer submission itself never waits.
        Sleep(10);
    }
    INFO("Trace output_failed=" << health.output_failed);
    REQUIRE(health.written == 1);
    const auto status = traces.status();
    REQUIRE(status.path);
    CHECK(status.path->extension() == ".etl");
    CHECK(std::filesystem::exists(*status.path));
}

TEST_CASE("tracing disabled by configuration validates definitions without storage or files") {
    TemporaryDirectory directory;
    fc::runtime::TraceSession traces;
    traces.configure(0, directory.path);
    constexpr std::string_view name = "Disabled";
    const FC_TraceDefinition definition{.struct_size = sizeof(FC_TraceDefinition),
                                        .name = {name.data(), static_cast<std::uint32_t>(name.size())},
                                        .capacity = 4,
                                        .max_record_size = 16,
                                        .version = 1};
    fc::runtime::PreparedTraceChannel preparation;
    FC_TraceHandle handle{};
    CHECK(traces.prepare_channel({0}, &definition, preparation, &handle) == FC_TRACE_DISABLED);
    CHECK(handle == nullptr);
    CHECK(traces.reserved_bytes() == 0);
    CHECK(traces.status().configured_disabled);
    CHECK_FALSE(std::filesystem::exists(directory.path / "traces"));
}

TEST_CASE("trace file cap terminalizes output and accounts queued plus later producer drops") {
    TemporaryDirectory directory;
    fc::reporting::ReportingSession reporting;
    reporting.start({.started = std::chrono::system_clock::now(), .role = FC_HOST_ROLE_CLIENT}, directory.path);
    reporting.configure(FC_LOG_DEBUG);
    fc::runtime::TraceSession traces;
    traces.configure(1, directory.path, reporting.logger("Tracing"));
    constexpr std::string_view name = "Capacity";
    const FC_TraceDefinition definition{.struct_size = sizeof(FC_TraceDefinition),
                                        .name = {name.data(), static_cast<std::uint32_t>(name.size())},
                                        .capacity = 100,
                                        .max_record_size = 48U * 1024U,
                                        .version = 1};
    fc::runtime::PreparedTraceChannel preparation;
    FC_TraceHandle handle{};
    REQUIRE(traces.prepare_channel({0}, &definition, preparation, &handle) == FC_TRACE_CREATED);
    traces.arm(preparation);
    const std::vector<std::uint8_t> record(definition.max_record_size, 0x5a);

    // Sustained accepted payloads let the writer reach the configured ETL cap without relying on an oversized record.
    for (unsigned attempt = 0; attempt < 400 && traces.enabled(handle) == FC_TRUE; ++attempt) {
        static_cast<void>(traces.try_write(handle, {record.data(), static_cast<std::uint32_t>(record.size())}));
        if (attempt % 8 == 0) {
            Sleep(5);
        }
    }
    FC_TraceHealth health{.struct_size = sizeof(FC_TraceHealth)};
    // After terminalization, wait for the consumer to account every previously accepted ring entry exactly once.
    for (unsigned attempt = 0; attempt < 400; ++attempt) {
        traces.health(handle, &health);
        if (health.file_limit_reached == FC_TRUE && health.written + health.dropped == health.accepted) {
            break;
        }
        Sleep(10);
    }
    REQUIRE(health.file_limit_reached == FC_TRUE);
    CHECK(health.output_failed == FC_FALSE);
    const auto dropped_before_terminal_write = health.dropped;
    CHECK(traces.try_write(handle, {record.data(), static_cast<std::uint32_t>(record.size())}) == FC_FALSE);
    traces.health(handle, &health);
    CHECK(health.dropped == dropped_before_terminal_write + 1);
    CHECK(health.written + health.dropped == health.accepted + 1);

    // The tracing owner reports its one terminal transition through the shared scoped log, not the producer path.
    reporting.publish(fc::reporting::InitializationStatus::Fatal, nullptr, nullptr, traces, true);
    const auto log = read_text(directory.path / "FusionCutter.log");
    CHECK(log.find("[FusionCutter/Tracing] Trace output reached its 1 MiB file limit") != std::string::npos);
}

TEST_CASE("trace output failure is terminal, visible in health, and logged by the tracing owner") {
    TemporaryDirectory directory;
    // A file at the required directory path injects a deterministic filesystem failure on the first accepted record.
    {
        std::ofstream blocker{directory.path / "traces", std::ios::binary};
        REQUIRE(blocker.put('x'));
    }
    fc::reporting::ReportingSession reporting;
    reporting.start({.started = std::chrono::system_clock::now(), .role = FC_HOST_ROLE_CLIENT}, directory.path);
    reporting.configure(FC_LOG_DEBUG);
    fc::runtime::TraceSession traces;
    traces.configure(8, directory.path, reporting.logger("Tracing"));
    constexpr std::string_view name = "Failure";
    const FC_TraceDefinition definition{.struct_size = sizeof(FC_TraceDefinition),
                                        .name = {name.data(), static_cast<std::uint32_t>(name.size())},
                                        .capacity = 2,
                                        .max_record_size = sizeof(std::uint32_t),
                                        .version = 1};
    fc::runtime::PreparedTraceChannel preparation;
    FC_TraceHandle handle{};
    REQUIRE(traces.prepare_channel({0}, &definition, preparation, &handle) == FC_TRACE_CREATED);
    traces.arm(preparation);
    const std::uint32_t payload = 42;
    REQUIRE(traces.try_write(handle, {reinterpret_cast<const std::uint8_t*>(&payload), sizeof(payload)}) == FC_TRUE);

    FC_TraceHealth health{.struct_size = sizeof(FC_TraceHealth)};
    for (unsigned attempt = 0; attempt < 200; ++attempt) {
        traces.health(handle, &health);
        if (health.output_failed == FC_TRUE) {
            break;
        }
        // The poll observes the asynchronous writer transition; producer submission itself remains nonblocking.
        Sleep(10);
    }
    REQUIRE(health.output_failed == FC_TRUE);
    reporting.publish(fc::reporting::InitializationStatus::Fatal, nullptr, nullptr, traces, true);
    const auto log = read_text(directory.path / "FusionCutter.log");
    CHECK(log.find("[FusionCutter/Tracing] Trace output directory could not be created") != std::string::npos);
}

TEST_CASE("crash annotations normalize structured text without splitting UTF-8") {
    TemporaryDirectory directory;
    fc::runtime::CrashPhaseCursors cursors;
    fc::runtime::CrashReporter crash{cursors};
    REQUIRE(crash.install(directory.path));

    fc::catalog::PluginRecord plugin;
    plugin.definition.id = "CrashFixture";
    // The final multibyte scalar straddles fixed storage while an earlier control byte tests line normalization.
    plugin.definition.version = "line\n" + std::string(89, 'v') + "\xc3\xa9";
    std::vector<fc::catalog::PluginRecord> plugins;
    plugins.push_back(std::move(plugin));
    const fc::catalog::Catalog catalog{std::move(plugins)};

    crash.publish_catalog(catalog);

    const auto snapshot = crash.snapshot();
    REQUIRE(snapshot.annotations.size() == 1);
    const std::string_view detail{snapshot.annotations.front().detail};
    CHECK(detail.size() == 94);
    CHECK(detail[4] == ' ');
    CHECK(detail.find('\n') == std::string_view::npos);
    CHECK(fc::catalog::valid_utf8(detail));
}

TEST_CASE("crash snapshots observe complete concurrent publication through the exact annotation capacity") {
    TemporaryDirectory directory;
    fc::runtime::CrashPhaseCursors cursors;
    fc::runtime::CrashReporter crash{cursors};
    REQUIRE(crash.install(directory.path));
    constexpr auto annotation_capacity = (512U * 1024U) / sizeof(fc::runtime::CrashAnnotation);
    static_assert(annotation_capacity > 2);

    // One plugin record plus one callback per patch leaves exactly one annotation slot for the boundary probe.
    auto owner = fc::catalog::CodeOwner::from_address(reinterpret_cast<std::uintptr_t>(&destroy_status_patch));
    REQUIRE(owner.has_value());
    fc::catalog::PluginRecord plugin;
    plugin.code_owner = std::move(*owner);
    plugin.definition.id = "CrashPublicationFixture";
    plugin.definition.version = "1";
    plugin.definition.patches.reserve(annotation_capacity - 2);
    for (std::size_t index = 0; index < annotation_capacity - 2; ++index) {
        fc::catalog::PatchDefinitionRecord patch;
        patch.id = "CrashPatch" + std::to_string(index);
        patch.selected_support = 0;
        patch.supports.push_back({.callbacks = {.destroy = &destroy_status_patch}});
        plugin.definition.patches.push_back(std::move(patch));
    }
    std::vector<fc::catalog::PluginRecord> plugins;
    plugins.push_back(std::move(plugin));
    const fc::catalog::Catalog catalog{std::move(plugins)};

    std::atomic_bool start{};
    std::atomic_bool published{};
    std::thread publisher{[&] {
        start.wait(false, std::memory_order_acquire);
        crash.publish_catalog(catalog);
        published.store(true, std::memory_order_release);
    }};
    start.store(true, std::memory_order_release);
    start.notify_one();

    // Repeated exceptional-reader snapshots must expose only monotonic, fully initialized immutable prefixes.
    std::size_t observed{};
    bool monotonic = true;
    bool complete_records = true;
    do {
        const auto snapshot = crash.snapshot();
        monotonic = monotonic && snapshot.annotations.size() >= observed;
        for (std::size_t index = observed; index < snapshot.annotations.size(); ++index) {
            const auto& annotation = snapshot.annotations[index];
            complete_records = complete_records && annotation.label[0] != '\0' && annotation.address != 0;
        }
        observed = std::max(observed, snapshot.annotations.size());
        std::this_thread::yield();
    } while (!published.load(std::memory_order_acquire));
    publisher.join();

    const auto bulk = crash.snapshot();
    CHECK(monotonic);
    CHECK(complete_records);
    REQUIRE(bulk.annotations.size() == annotation_capacity - 1);
    CHECK(bulk.omitted == 0);

    // The empty built-in plugin contributes one valid record at the limit; its repetition is the first omission.
    auto core_owner = fc::catalog::CodeOwner::from_address(reinterpret_cast<std::uintptr_t>(&destroy_status_patch));
    REQUIRE(core_owner.has_value());
    fc::catalog::PluginRecord core;
    core.code_owner = std::move(*core_owner);
    core.definition.id = "Core";
    std::vector<fc::catalog::PluginRecord> core_plugins;
    core_plugins.push_back(std::move(core));
    const fc::catalog::Catalog boundary{std::move(core_plugins)};
    crash.publish_catalog(boundary);
    CHECK(crash.snapshot().annotations.size() == annotation_capacity);
    CHECK(crash.snapshot().omitted == 0);
    crash.publish_catalog(boundary);
    CHECK(crash.snapshot().annotations.size() == annotation_capacity);
    CHECK(crash.snapshot().omitted == 1);
}

TEST_CASE("crash capture excludes expected faults and bounds replacement plus append attempts") {
    TemporaryDirectory directory;
    fc::runtime::CrashPhaseCursors cursors;
    fc::runtime::CrashReporter crash{cursors};
    REQUIRE(crash.install(directory.path));
    constexpr DWORD exception = 0xc0000005U;

    {
        fc::runtime::CrashReporter::ExpectedFaultScope expected{exception};
        raise_caught(exception);
    }
    CHECK_FALSE(std::filesystem::exists(directory.path / "FusionCutter.Crash.log"));

    std::atomic_bool guarded_thread_ready{};
    std::atomic_bool release_guarded_thread{};
    std::thread guarded_thread{[&] {
        fc::runtime::CrashReporter::ExpectedFaultScope expected{exception};
        guarded_thread_ready.store(true, std::memory_order_release);
        guarded_thread_ready.notify_one();
        release_guarded_thread.wait(false, std::memory_order_acquire);
        raise_caught(exception);
    }};
    guarded_thread_ready.wait(false, std::memory_order_acquire);
    // A guarded read on another thread must not hide this qualifying first-chance exception.
    raise_caught(exception);
    release_guarded_thread.store(true, std::memory_order_release);
    release_guarded_thread.notify_one();
    guarded_thread.join();

    for (unsigned attempt = 1; attempt < 4; ++attempt) {
        raise_caught(exception);
    }
    const auto report_path = directory.path / "FusionCutter.Crash.log";
    REQUIRE(std::filesystem::exists(report_path));
    const auto size_after_four = std::filesystem::file_size(report_path);
    const auto report = read_text(report_path);
    CHECK(report.find("Report: 1 of 4") != std::string::npos);
    CHECK(report.find("Report: 4 of 4") != std::string::npos);
    CHECK(report.find("first-chance") != std::string::npos);
    CHECK(report.find("(ACCESS_VIOLATION)") != std::string::npos);
    CHECK(report.find("Stack frames:") != std::string::npos);
    CHECK(report.find("Stack window:") != std::string::npos);
    CHECK(report.find("Implicated modules:") != std::string::npos);

    // The fifth qualifying exception remains observable by downstream handlers but cannot grow the crash file.
    raise_caught(exception);
    CHECK(std::filesystem::file_size(report_path) == size_after_four);
}
