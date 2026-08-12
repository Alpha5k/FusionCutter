#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace fusioncutter::patches::spectator_camera {

enum class TransformPath : std::uint8_t {
    Object,
    Camera,
};

enum class HistoryUpdate : std::uint8_t {
    Pending,
    Added,
    Replaced,
    ResetOwner,
    ResetGap,
    ResetIncomplete,
    ResetDiscontinuity,
    Stale,
    Invalid,
};

enum class SmoothingResult : std::uint8_t {
    Smoothed,
    Ramping,
    HistoryWarmup,
    PhaseMismatch,
    Invalid,
};

// Smooths paired object and camera translations with a one-update-delayed 1:2:1 filter and gradual blend-in.
class TransformSmoother {
  public:
    static constexpr float kMaximumStepDistance = 4.0F;
    static constexpr float kPhaseTolerance = 0.125F;
    static constexpr std::uint8_t kRampTurns = 4;

    // Pair the object and camera endpoints published for each authoritative update turn.
    HistoryUpdate publish_object(void* owner, std::int32_t turn, const float* matrix) noexcept;
    HistoryUpdate publish_camera(void* owner, std::int32_t turn, const float* matrix) noexcept;
    // Filter translation only after proving the native render output is still on the expected interpolation phase.
    SmoothingResult smooth(TransformPath path, float ratio, float* matrix) const noexcept;

    void reset() noexcept;
    [[nodiscard]] bool ready(void* owner) const noexcept;

    [[nodiscard]] std::size_t count() const noexcept;

  private:
    using Position = std::array<float, 3>;

    struct Frame {
        Position object{};
        Position camera{};
        std::int32_t turn{-1};
    };

    struct PendingFrame : Frame {
        std::uint8_t paths{};
    };

    [[nodiscard]] HistoryUpdate publish(TransformPath path, void* owner, std::int32_t turn,
                                        const float* matrix) noexcept;
    void begin_pending(std::int32_t turn) noexcept;
    [[nodiscard]] HistoryUpdate commit_pending() noexcept;
    void clear_history() noexcept;

    [[nodiscard]] static constexpr std::uint8_t path_mask(TransformPath path) noexcept;
    [[nodiscard]] static Position& position(Frame& frame, TransformPath path) noexcept;
    [[nodiscard]] static const Position& position(const Frame& frame, TransformPath path) noexcept;
    [[nodiscard]] static bool is_next(std::int32_t previous, std::int32_t current) noexcept;
    [[nodiscard]] static bool is_older(std::int32_t current, std::int32_t reference) noexcept;
    [[nodiscard]] static bool read_position(Position& output, const float* matrix) noexcept;
    static void write_position(const Position& position, float* matrix) noexcept;
    [[nodiscard]] static Position interpolate(const Position& start, const Position& end, float ratio) noexcept;
    [[nodiscard]] static Position weighted_average(const Position& first, const Position& middle,
                                                   const Position& last) noexcept;
    [[nodiscard]] static float distance_squared(const Position& left, const Position& right) noexcept;
    [[nodiscard]] static bool discontinuous(const Position& previous, const Position& current) noexcept;

    std::array<Frame, 4> frames_{};
    PendingFrame pending_{};
    void* owner_{};
    std::size_t count_{};
    std::uint8_t age_{};
};

} // namespace fusioncutter::patches::spectator_camera
