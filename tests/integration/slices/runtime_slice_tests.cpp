#include "runtime_contract.h"

#include <catch2/catch_test_macros.hpp>

#include <Windows.h>

#include <process.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

namespace {

// Every child process receives a complete private installation so configuration and output cannot cross test rows.
class TemporaryDirectory final {
  public:
    TemporaryDirectory() {
        static std::atomic_uint64_t sequence{};
        path = std::filesystem::temp_directory_path() /
               ("FusionCutter-RuntimeSlice-" + std::to_string(GetCurrentProcessId()) + "-" +
                std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)));
        REQUIRE(std::filesystem::create_directory(path));
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }

    std::filesystem::path path;
};

// One row varies optional activation order and the controlled observer failure without changing fixture artifacts.
struct RuntimeCase {
    std::string_view name;
    bool early_binding{};
    bool fail_observer{};
};

// Copied process outputs let assertions cover runtime state after every DLL has left the child process.
struct RuntimeResult {
    int exit_code{};
    std::string status;
    std::string configuration;
    std::string log;
    std::uintmax_t trace_bytes{};
};

[[nodiscard]] std::string read_text(const std::filesystem::path& path) {
    std::ifstream input{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

void copy_artifact(const std::filesystem::path& source, const std::filesystem::path& destination) {
    REQUIRE(std::filesystem::copy_file(source, destination));
}

// Stages the three real plugins and drives their lifecycle only through the published CoreApi child boundary.
[[nodiscard]] RuntimeResult run_runtime_case(const std::filesystem::path& directory, const RuntimeCase& test_case) {
    // Stage one isolated installation containing the real framework, native host, and three external plugins.
    REQUIRE(std::filesystem::create_directory(directory));
    REQUIRE(std::filesystem::create_directory(directory / "plugins"));
    REQUIRE(std::filesystem::create_directory(directory / "config"));
    const auto host = directory / "FusionCutterSliceHost.exe";
    copy_artifact(FC_SLICE_HOST_PATH, host);
    copy_artifact(FC_SLICE_CORE_PATH, directory / "FusionCutter.dll");
    copy_artifact(FC_RUNTIME_PROVIDER_PATH, directory / "plugins" / "fc_runtime_provider_slice.dll");
    copy_artifact(FC_RUNTIME_OBSERVER_PATH, directory / "plugins" / "fc_runtime_observer_slice.dll");
    copy_artifact(FC_RUNTIME_LATE_PARTICIPANT_PATH, directory / "plugins" / "fc_runtime_late_participant_slice.dll");
#if defined(_M_IX86)
    copy_artifact(FC_SLICE_LATE_IMAGE_PATH, directory / "FusionCutterSlicePeer.dll");
#endif

    // Enable Debug logging and traces, then apply only the settings that differ for this table row.
    {
        std::ofstream output{directory / "config" / "FC.Core.ini", std::ios::binary};
        REQUIRE(output << "[FusionCutter]\r\nLogLevel=Debug\r\nMaxTraceSizeMB=8\r\n");
    }
    {
        std::ofstream output{directory / "config" / "FC.RuntimeObserverSlice.ini", std::ios::binary};
        REQUIRE(output << "[General]\r\nRuntimeEarlyBinding=" << (test_case.early_binding ? "true" : "false")
                       << "\r\nRuntimeObserver=true\r\n\r\n[RuntimeObserver.Behavior]\r\nFailPrepare="
                       << (test_case.fail_observer ? "true" : "false")
                       << "\r\n\r\n[RuntimeObserver.Tracing]\r\nCapacity=16\r\nMaximumRecordSize=64\r\n");
    }

    // The native child owns all mutation and pumping; the test process reads only completed output artifacts.
    const auto exit_code = static_cast<int>(_wspawnl(_P_WAIT, host.c_str(), host.c_str(), nullptr));
    std::uintmax_t trace_bytes{};
    const auto trace_directory = directory / "traces";
    if (std::filesystem::exists(trace_directory)) {
        for (const auto& entry : std::filesystem::recursive_directory_iterator{trace_directory}) {
            if (entry.is_regular_file()) {
                trace_bytes += entry.file_size();
            }
        }
    }
    return {exit_code, read_text(directory / "FusionCutter.txt"),
            read_text(directory / "config" / "FC.RuntimeObserverSlice.ini"), read_text(directory / "FusionCutter.log"),
            trace_bytes};
}

} // namespace

TEST_CASE(
    "runtime slice with multiple plugins crosses native, lifecycle, interface, trace, and late image boundaries") {
    TemporaryDirectory directory;
    constexpr std::array cases{
        RuntimeCase{"provider active before consumer", false, false},
        RuntimeCase{"consumer waits for provider activation", true, false},
        RuntimeCase{"observer Prepare callback failure", false, true},
    };

    for (const auto& test_case : cases) {
        DYNAMIC_SECTION(test_case.name) {
            const auto result = run_runtime_case(directory.path / test_case.name, test_case);
            CAPTURE(test_case.early_binding, test_case.fail_observer, result.status, result.log);

            // Common assertions prove initialization, provider activation, retained Original use, and scoped logging.
            REQUIRE(result.exit_code == 0);
            CHECK(result.status.find("Initialization: Completed") != std::string::npos);
#if defined(_M_IX86)
            CHECK(result.status.find("Target: GOG Retail (Server, x86)") != std::string::npos);
#else
            CHECK(result.status.find("Target: Classic Collection (Server, x64)") != std::string::npos);
#endif
            CHECK(result.status.find("RuntimeProviderSlice (1.0.0)") != std::string::npos);
            CHECK(result.status.find("Retained original result: " +
                                     std::to_string(FC_SLICE_ACTIVATE_ORIGINAL_RESULT)) != std::string::npos);
            CHECK(result.log.find("[RuntimeProviderSlice/RuntimeProvider] Planned ownership of the mixed native call "
                                  "and instruction sites") != std::string::npos);

            // Failure proves local pruning; successful rows prove observation, interfaces, and trace transport.
            if (test_case.fail_observer) {
                CHECK(result.status.find("Failed patches:") != std::string::npos);
                CHECK(result.status.find("Runtime observer (RuntimeObserverSlice/RuntimeObserver)") !=
                      std::string::npos);
                CHECK(result.status.find("Phase: Prepare") != std::string::npos);
                CHECK(result.status.find("Skipped patches:") != std::string::npos);
                CHECK(result.status.find("Runtime observer consumer "
                                         "(RuntimeObserverSlice/RuntimeObserverConsumer)") != std::string::npos);
                CHECK(result.trace_bytes == 0);
            } else {
                CHECK(result.status.find("Before calls: 257") != std::string::npos);
                CHECK(result.status.find("After calls: 257") != std::string::npos);
                CHECK(result.status.find("State mismatches: 0") != std::string::npos);
                CHECK(result.status.find("Immediate interface result: 103") != std::string::npos);
                CHECK(result.status.find("Bound interface result: 104") != std::string::npos);
                CHECK(result.status.find("Trace records accepted:") != std::string::npos);
                CHECK(result.trace_bytes > 0);
            }

            if (test_case.early_binding) {
                CHECK(result.status.find("Runtime early binding "
                                         "(RuntimeObserverSlice/RuntimeEarlyBinding):") != std::string::npos);
                CHECK(result.status.find("Bound interface result: 106") != std::string::npos);
            }
#if defined(_M_IX86)
            // The x86 server row also proves image arrival, late site joining, and immediate optional binding.
            CHECK(result.status.find("Runtime late owner (RuntimeProviderSlice/RuntimeLateOwner):") !=
                  std::string::npos);
            CHECK(result.status.find("Runtime late participant "
                                     "(RuntimeLateParticipantSlice/RuntimeLateParticipant):") != std::string::npos);
            CHECK(result.status.find("Bound interface result: 105") != std::string::npos);
#endif
            CHECK(result.configuration.find("[RuntimeObserver.Behavior]") != std::string::npos);
            CHECK(result.configuration.find("[RuntimeObserver.Tracing]") != std::string::npos);
        }
    }
}
