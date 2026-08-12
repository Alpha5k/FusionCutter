#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace fusioncutter::patches::spectator_camera {

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

// Smooths the rendered spectator object from confirmed network updates.
class TransformSmoother {
  public:
    static constexpr float kMaximumStepDistance = 4.0F;
    static constexpr float kPhaseTolerance = 0.125F;
    static constexpr std::uint8_t kRampTurns = 4;

    // Admit an authoritative object position only after its matching spectator camera is confirmed.
    HistoryUpdate publish_object(void* owner, std::int32_t turn, const float* matrix) noexcept;
    HistoryUpdate confirm_camera(void* owner, std::int32_t turn) noexcept;
    // Replace the native object translation with the filtered position.
    SmoothingResult smooth_object(float ratio, float* matrix) const noexcept;

    void reset() noexcept;

  private:
    using Position = std::array<float, 3>;

    enum class Publication : std::uint8_t {
        Object,
        CameraConfirmation,
    };

    struct Frame {
        Position position{};
        std::int32_t turn{-1};
    };

    struct PendingFrame : Frame {
        std::uint8_t publications{};
    };

    struct TranslationCorrection {
        Position expected_position{};
        Position translation{};
        float blend_weight{};
    };

    [[nodiscard]] HistoryUpdate publish(Publication publication, void* owner, std::int32_t turn,
                                        const float* matrix) noexcept;
    void begin_pending(std::int32_t turn) noexcept;
    [[nodiscard]] HistoryUpdate commit_pending() noexcept;
    void clear_history() noexcept;
    [[nodiscard]] SmoothingResult calculate_correction(float ratio, TranslationCorrection& output) const noexcept;

    [[nodiscard]] static constexpr std::uint8_t publication_mask(Publication publication) noexcept;
    [[nodiscard]] static bool is_next(std::int32_t previous, std::int32_t current) noexcept;
    [[nodiscard]] static bool is_older(std::int32_t current, std::int32_t reference) noexcept;
    [[nodiscard]] static bool read_position(Position& output, const float* matrix) noexcept;
    static void write_position(const Position& position, float* matrix) noexcept;
    static void apply_translation(Position& position, const Position& translation) noexcept;
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
