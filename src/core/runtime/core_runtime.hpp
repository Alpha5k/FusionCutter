#pragma once

#include "../reporting/crash_reporter.hpp"
#include "patch_runtime.hpp"
#include "../reporting/tracing.hpp"
#include "../reporting/reporting.hpp"

#include <FusionCutter/CoreApi.h>

#include <cstdint>
#include <expected>
#include <limits>
#include <optional>
#include <string>

namespace fc::runtime {

// These owned C++ values mirror the loader ABI only after its raw scalars have passed tuple validation.
enum class InitializationResult : std::uint32_t {
    Completed = FC_INIT_COMPLETED,
    Unsupported = FC_INIT_UNSUPPORTED,
    Fatal = FC_INIT_FATAL,
};

enum class LoaderKind : std::uint32_t {
    Unknown = FC_LOADER_KIND_UNKNOWN,
    DirectInput8 = FC_LOADER_KIND_DINPUT8,
    RconServer = FC_LOADER_KIND_RCONSERVER,
    Battlefront2 = FC_LOADER_KIND_BATTLEFRONT2,
};

enum class DirectInputChainResult : std::uint32_t {
    NotApplicable = FC_DIRECT_INPUT_CHAIN_NOT_APPLICABLE,
    SystemOnly = FC_DIRECT_INPUT_CHAIN_SYSTEM_ONLY,
    Loaded = FC_DIRECT_INPUT_CHAIN_LOADED,
    Invalid = FC_DIRECT_INPUT_CHAIN_INVALID,
    Ambiguous = FC_DIRECT_INPUT_CHAIN_AMBIGUOUS,
};

// The loader's fixed C record is copied into framework-owned values before initialization uses it.
struct LoaderStartupRecord {
    LoaderKind kind{};
    DirectInputChainResult direct_input_chain{};
    std::uint32_t windows_error{};
    std::uint32_t wildcard_match_count{};
    std::string selected_proxy_basename;
};

// Initialization retains the host role together with every loader-owned startup fact needed by later reporting.
struct InitializationRequest {
    FC_HostRole role{};
    LoaderStartupRecord loader;
};

// The first terminal result remains authoritative for every repeated Initialize call from a shipping loader.
struct InitializationRecord {
    InitializationResult result{};
    std::optional<planning::FailureReason> reason;
};

// Owns the one forward-only framework lifecycle and every service that installed callbacks may retain.
class CoreRuntime final {
  public:
    CoreRuntime() noexcept;

    [[nodiscard]] InitializationResult initialize(const InitializationRequest& request);
    [[nodiscard]] InitializationResult reject_initialization(std::string reason) noexcept;
    [[nodiscard]] std::optional<InitializationResult> initialization_result() const noexcept;
    void update() noexcept;

    [[nodiscard]] const FC_HostApi& host_api() const noexcept;
    [[nodiscard]] const CrashReporter& crash_reporter() const noexcept;
    [[nodiscard]] const TraceSession& traces() const noexcept;

  private:
    [[nodiscard]] InitializationResult finish(InitializationResult result,
                                              std::optional<planning::FailureReason> reason = {});
    [[nodiscard]] static FC_Bool FC_CALL log_enabled(void* context, FC_ReportToken report, FC_LogLevel level) noexcept;
    static void FC_CALL log_write(void* context, FC_ReportToken report, FC_LogLevel level,
                                  FC_StringView message) noexcept;
    [[nodiscard]] static FC_Bool FC_CALL trace_enabled(void* context, FC_TraceHandle trace) noexcept;
    [[nodiscard]] static FC_Bool FC_CALL trace_try_write(void* context, FC_TraceHandle trace,
                                                         FC_ByteView record) noexcept;
    static void FC_CALL trace_health(void* context, FC_TraceHandle trace, FC_TraceHealth* output) noexcept;
    static void fatal_handler(void* context, std::string_view reason) noexcept;

    std::optional<InitializationRecord> initialization_;
    std::optional<InitializationRequest> request_;
    CrashPhaseCursors crash_phases_;
    reporting::ReportingSession reporting_;
    TraceSession traces_;
    CrashReporter crash_;
    FC_HostApi host_api_{};

    // Destroyed first in test teardown; production deliberately retains the containing CoreRuntime for process life.
    std::optional<PatchRuntimeState> patch_runtime_;
};

// Validates the loader tuple for ABI generation 1 and copies every borrowed field before CoreRuntime sees it.
[[nodiscard]] std::expected<InitializationRequest, std::string>
copy_initialization_request(const FC_InitializeArgs* arguments);

} // namespace fc::runtime
