#pragma once

#include "../catalog/catalog_types.hpp"
#include "../planning/planning_types.hpp"
#include "../targets/recognition.hpp"

#include <FusionCutter/PluginApi.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fc::runtime {
struct PatchRuntimeState;
}

namespace fc::reporting {

// The status renderer owns a small terminal vocabulary independent of CoreRuntime's loader API.
enum class InitializationStatus {
    Completed,
    Unsupported,
    Fatal,
};

// These immutable session facts are copied before any loader-owned storage can expire.
struct SessionFacts {
    std::chrono::system_clock::time_point started;
    FC_HostRole role{};
    std::uint32_t loader_kind{};
    std::uint32_t direct_input_chain{};
    std::string selected_proxy_basename;
    std::string executable_basename;
    std::optional<std::uint32_t> executable_timestamp;
    std::optional<std::uint64_t> executable_image_size;
};

// Logging state is sampled atomically and rendered without calling the asynchronous backend.
struct LogStatus {
    FC_LogLevel level{};
    std::filesystem::path path;
    std::uint64_t accepted{};
    std::uint64_t written{};
    std::uint64_t dropped{};
    bool output_failed{};
};

// Trace state is an aggregate diagnostic; individual channel counters remain available through the SDK.
struct TraceStatus {
    bool requested{};
    bool configured_disabled{};
    std::optional<std::filesystem::path> path;
    std::uint64_t dropped{};
    bool file_limit_reached{};
    bool output_failed{};
};

// Each accepted field is already normalized and charged against one patch's bounded live section.
struct StatusField {
    std::string label;
    std::string value;
};

struct LiveStatusSection {
    catalog::PatchIndex patch;
    std::vector<StatusField> fields;
    std::size_t omitted{};
    bool callback_failed{}; // Lets reporting diagnose a caught callback failure without calling plugin code again.
};

// Publication results let the reporting owner log direct write failures without coupling status to logging.
enum class StatusPublishResult {
    Deferred,
    Unchanged,
    Written,
    Failed,
};

// Owns the last successfully published text and all copied inputs needed to retry a failed direct write.
class StatusPublisher final {
  public:
    explicit StatusPublisher(std::filesystem::path path);

    void set_session(SessionFacts facts);
    void set_target(const targets::RecognizedTarget& target);
    void set_catalog(const catalog::Catalog& catalog, std::span<const catalog::RejectionRecord> rejections,
                     std::filesystem::path core_configuration);

    // Live callbacks execute only during collection; render() consumes the resulting owned fields later.
    [[nodiscard]] std::vector<LiveStatusSection> collect_live(const runtime::PatchRuntimeState& runtime) const;

    // Reporting checks the cadence before invoking plugin status callbacks, then publish records the opportunity.
    [[nodiscard]] bool publication_due(bool force) const noexcept;

    // A forced startup publication bypasses the one-second cadence but still avoids an unchanged rewrite.
    [[nodiscard]] StatusPublishResult publish(InitializationStatus initialization,
                                              const planning::FailureReason* reason,
                                              const runtime::PatchRuntimeState* runtime,
                                              std::span<const LiveStatusSection> live, const LogStatus& log,
                                              const TraceStatus& trace, bool force) noexcept;

  private:
    [[nodiscard]] std::string render(InitializationStatus initialization, const planning::FailureReason* reason,
                                     const runtime::PatchRuntimeState* runtime, std::span<const LiveStatusSection> live,
                                     const LogStatus& log, const TraceStatus& trace) const;

    std::filesystem::path path_;
    std::optional<SessionFacts> session_;
    const targets::RecognizedTarget* target_{};
    const catalog::Catalog* catalog_{};
    std::vector<catalog::RejectionRecord> rejections_;
    std::filesystem::path core_configuration_;
    std::string last_successful_output_;
    std::chrono::steady_clock::time_point last_attempt_{};
};

} // namespace fc::reporting
