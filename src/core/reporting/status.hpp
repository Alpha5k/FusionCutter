#pragma once

#include "session.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace fusioncutter::reporting {

struct LogStatus {
    LogLevel level;
    std::filesystem::path filename;
    std::uint64_t accepted_entries;
    std::uint64_t written_entries;
    bool output_failed;
};

class StatusPublisher {
  public:
    explicit StatusPublisher(std::filesystem::path path);

    void set_target(const TargetContext& target);
    void set_configuration(const std::filesystem::path& path);
    void set_snapshot(const InitializationResult& initialization, std::span<const PatchResult> patch_results,
                      std::span<const StatusContributorRef> contributors);

    void publish_now(const LogStatus& log_status) noexcept;
    void publish_if_due(const LogStatus& log_status) noexcept;

    struct StoredReason {
        std::string message;
        std::optional<std::string> operation;
        std::optional<std::string> related_patch;
    };

    struct StoredPatchResult {
        std::string name;
        PatchOutcome outcome;
        std::optional<StoredReason> reason;
    };

    struct StoredContributor {
        std::string name;
        const StatusContributor* contributor;
    };

  private:
    [[nodiscard]] std::string render(const LogStatus& log_status) const;
    void publish(const LogStatus& log_status, bool force) noexcept;

    std::filesystem::path path_;
    mutable std::mutex mutex_;
    std::optional<TargetContext> target_;
    std::optional<std::filesystem::path> configuration_;
    InitializationOutcome initialization_{InitializationOutcome::Fatal};
    std::optional<StoredReason> initialization_reason_;
    std::vector<StoredPatchResult> patch_results_;
    std::vector<StoredContributor> contributors_;
    std::string previous_output_;
    std::chrono::steady_clock::time_point last_publication_{};
};

} // namespace fusioncutter::reporting
