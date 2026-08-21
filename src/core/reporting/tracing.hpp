#pragma once

#include "../catalog/catalog_types.hpp"
#include "../core_logger.hpp"
#include "../reporting/status.hpp"

#include <FusionCutter/PluginApi.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace fc::runtime {

class TraceSession;
struct TraceChannelState;

// One attempt token releases an unpublished channel unless successful activation arms it for process lifetime.
class PreparedTraceChannel final {
  public:
    PreparedTraceChannel() = default;
    PreparedTraceChannel(const PreparedTraceChannel&) = delete;
    PreparedTraceChannel& operator=(const PreparedTraceChannel&) = delete;
    PreparedTraceChannel(PreparedTraceChannel&& other) noexcept;
    PreparedTraceChannel& operator=(PreparedTraceChannel&& other) noexcept;
    ~PreparedTraceChannel();

  private:
    PreparedTraceChannel(TraceSession& session, TraceChannelState& channel) noexcept;
    void release() noexcept;

    TraceSession* session_{};
    TraceChannelState* channel_{};

    friend class TraceSession;
};

// Owns bounded channel storage and the host callbacks used by all admitted plugins.
class TraceSession final {
  public:
    explicit TraceSession(std::uint32_t max_trace_size_mb = 512) noexcept;
    TraceSession(const TraceSession&) = delete;
    TraceSession& operator=(const TraceSession&) = delete;
    ~TraceSession();

    // Configuration may replace the compiled default once, before any plugin receives a channel handle.
    void configure(std::uint32_t max_trace_size_mb, std::filesystem::path installation_directory = {},
                   CoreLogger logger = {}) noexcept;
    // Publishing the plugin catalog gives the writer stable names before any prepared channel can be armed.
    void set_catalog(const catalog::Catalog& catalog) noexcept;

    // Creation validates and reserves all producer storage while the patch remains in its fallible Prepare phase.
    [[nodiscard]] FC_TraceCreateResult prepare_channel(catalog::PatchIndex patch, const FC_TraceDefinition* definition,
                                                       PreparedTraceChannel& preparation,
                                                       FC_TraceHandle* output) noexcept;

    // Activation publishes prepared producer state; failure cleanup instead lets the attempt token release it.
    void arm(PreparedTraceChannel& preparation) noexcept;

    [[nodiscard]] FC_Bool enabled(FC_TraceHandle handle) const noexcept;
    [[nodiscard]] FC_Bool try_write(FC_TraceHandle handle, FC_ByteView record) noexcept;
    void health(FC_TraceHandle handle, FC_TraceHealth* output) const noexcept;

    [[nodiscard]] std::size_t channel_count() const noexcept;
    [[nodiscard]] std::size_t reserved_bytes() const noexcept;
    // The status publisher receives aggregate health and the lazy file path, never mutable writer internals.
    [[nodiscard]] reporting::TraceStatus status() const noexcept;

  private:
    // Only an unarmed attempt may release a reservation; the handle table rejects stale or fabricated tokens.
    void release(TraceChannelState& channel) noexcept;
    [[nodiscard]] bool initialize_handle_table() noexcept;
    [[nodiscard]] TraceChannelState* find(FC_TraceHandle handle) noexcept;
    [[nodiscard]] const TraceChannelState* find(FC_TraceHandle handle) const noexcept;
    void notify_writer() noexcept;

    class Writer;

    std::uint32_t max_trace_size_mb_{};
    std::filesystem::path installation_directory_;
    CoreLogger logger_;
    const catalog::Catalog* catalog_{};
    std::size_t reserved_bytes_{};
    mutable std::mutex channels_mutex_;
    std::vector<std::unique_ptr<TraceChannelState>> channels_;
    // Declaring the writer after channel storage stops its consumer thread before those channels are destroyed.
    std::unique_ptr<Writer> writer_;
    std::atomic_bool requested_{};
    // The fixed table is allocated with the first enabled channel and never moves while producers may use it.
    // A generation distinguishes each use of a slot so a released handle cannot name its replacement.
    std::unique_ptr<std::atomic<TraceChannelState*>[]> handle_slots_;
    std::unique_ptr<std::atomic<std::uint32_t>[]> handle_generations_;
    std::unique_ptr<std::uint32_t[]> free_handle_ids_;
    std::size_t handle_capacity_{};
    std::size_t free_handle_count_{};
    std::uint32_t next_channel_id_{};

    friend class PreparedTraceChannel;
};

} // namespace fc::runtime
