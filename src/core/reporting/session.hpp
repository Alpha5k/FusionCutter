#pragma once

#include <FusionCutter/outcome.hpp>
#include <FusionCutter/reporting.hpp>
#include <FusionCutter/target.hpp>

#include <filesystem>
#include <span>
#include <string_view>

namespace fusioncutter::reporting {

struct StatusContributorRef {
    std::string_view name;
    const StatusContributor* contributor;
};

class Session {
  public:
    [[nodiscard]] static Session& instance() noexcept;

    void start(HostRole role) noexcept;
    void set_level(LogLevel level) noexcept;
    void set_target(const TargetContext& target) noexcept;
    void set_configuration(const std::filesystem::path& path) noexcept;
    void publish_status(const InitializationResult& initialization, std::span<const PatchResult> patch_results = {},
                        std::span<const StatusContributorRef> contributors = {}) noexcept;
    void flush() noexcept;

    [[nodiscard]] bool enabled(LogLevel level) const noexcept;
    void write(LogLevel level, PatchId source, std::string_view message, std::string_view operation,
               PatchId related_patch) noexcept;

  private:
    class State;

    Session();

    State* state_;
};

} // namespace fusioncutter::reporting
