#pragma once

#include "status.hpp"

#include "../core_logger.hpp"

#include <FusionCutter/PluginApi.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string_view>

namespace fc::runtime {
class TraceSession;
struct PatchRuntimeState;
} // namespace fc::runtime

namespace fc::reporting {

// ReportingSession owns diagnostic output while keeping callbacks exposed to plugins small and nonthrowing.
class ReportingSession final {
  public:
    ReportingSession();
    ReportingSession(const ReportingSession&) = delete;
    ReportingSession& operator=(const ReportingSession&) = delete;
    ~ReportingSession();

    // Startup resolves installation-local output paths and publishes the configured filter before plugin callbacks.
    void start(SessionFacts facts, std::filesystem::path output_directory = {}) noexcept;
    void configure(FC_LogLevel level) noexcept;
    void set_target(const targets::RecognizedTarget& target) noexcept;
    void set_catalog(const catalog::Catalog& catalog, std::span<const catalog::RejectionRecord> rejections) noexcept;

    // Framework components receive a scoped capability rather than depending on Quill or a global reporting locator.
    [[nodiscard]] CoreLogger logger(std::string_view scope) noexcept;

    [[nodiscard]] FC_Bool enabled(FC_ReportToken report, FC_LogLevel level) const noexcept;
    void write(FC_ReportToken report, FC_LogLevel level, FC_StringView message) noexcept;

    // Initial publication collects live state once; periodic publication follows the Update callback on the same pump.
    void publish(InitializationStatus initialization, const planning::FailureReason* reason,
                 const runtime::PatchRuntimeState* runtime, const runtime::TraceSession& traces, bool force) noexcept;

    // Fatal reporting never calls a plugin; it reuses the latest live snapshot and attempts one bounded flush.
    void fatal(std::string_view reason, const runtime::PatchRuntimeState* runtime,
               const runtime::TraceSession& traces) noexcept;

  private:
    class State;
    std::unique_ptr<State> state_;
};

} // namespace fc::reporting
