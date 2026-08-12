#include "smoothing.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace fusioncutter::patches::spectator_camera {
namespace {

constexpr std::uint8_t kCompletePublicationSet = 3U;

} // namespace

HistoryUpdate TransformSmoother::publish_object(void* owner, std::int32_t turn, const float* matrix) noexcept {
    return publish(Publication::Object, owner, turn, matrix);
}

HistoryUpdate TransformSmoother::confirm_camera(void* owner, std::int32_t turn) noexcept {
    return publish(Publication::CameraConfirmation, owner, turn, nullptr);
}

SmoothingResult TransformSmoother::smooth_object(float ratio, float* matrix) const noexcept {
    Position native{};
    if (!read_position(native, matrix)) {
        return SmoothingResult::Invalid;
    }

    TranslationCorrection correction{};
    const auto result = calculate_correction(ratio, correction);
    if (result == SmoothingResult::Invalid || result == SmoothingResult::HistoryWarmup) {
        return result;
    }

    const float phase_error_squared = distance_squared(correction.expected_position, native);
    if (phase_error_squared > kPhaseTolerance * kPhaseTolerance) {
        return SmoothingResult::PhaseMismatch;
    }

    apply_translation(native, correction.translation);
    write_position(native, matrix);
    return result;
}

void TransformSmoother::reset() noexcept {
    frames_ = {};
    pending_ = {};
    owner_ = nullptr;
    count_ = 0;
    age_ = 0;
}

HistoryUpdate TransformSmoother::publish(Publication publication, void* owner, std::int32_t turn,
                                         const float* matrix) noexcept {
    Position sample{};
    if (owner == nullptr || turn < 0 || (publication == Publication::Object && !read_position(sample, matrix))) {
        return HistoryUpdate::Invalid;
    }

    auto result = HistoryUpdate::Pending;
    if (owner_ != owner) {
        reset();
        owner_ = owner;
        result = HistoryUpdate::ResetOwner;
    }

    if (count_ != 0 && turn == frames_[count_ - 1].turn) {
        if (publication == Publication::CameraConfirmation) {
            return HistoryUpdate::Replaced;
        }

        frames_[count_ - 1].position = sample;
        if (count_ > 1 && discontinuous(frames_[count_ - 2].position, sample)) {
            const auto latest = frames_[count_ - 1];
            frames_ = {};
            frames_.front() = latest;
            pending_ = {};
            count_ = 1;
            age_ = 1;
            return HistoryUpdate::ResetDiscontinuity;
        }
        return HistoryUpdate::Replaced;
    }

    if (pending_.publications != 0) {
        if (turn != pending_.turn) {
            if (is_older(turn, pending_.turn)) {
                return HistoryUpdate::Stale;
            }
            clear_history();
            result = HistoryUpdate::ResetIncomplete;
            begin_pending(turn);
        }
    } else {
        if (count_ != 0 && !is_next(frames_[count_ - 1].turn, turn)) {
            if (is_older(turn, frames_[count_ - 1].turn)) {
                return HistoryUpdate::Stale;
            }
            clear_history();
            result = HistoryUpdate::ResetGap;
        }
        begin_pending(turn);
    }

    const auto mask = publication_mask(publication);
    const bool replaced = (pending_.publications & mask) != 0;
    if (publication == Publication::Object) {
        pending_.position = sample;
    }
    pending_.publications = static_cast<std::uint8_t>(pending_.publications | mask);
    if (pending_.publications != kCompletePublicationSet) {
        return replaced ? HistoryUpdate::Replaced : result;
    }

    const auto committed = commit_pending();
    if (committed != HistoryUpdate::Added) {
        return committed;
    }
    return result == HistoryUpdate::Pending ? HistoryUpdate::Added : result;
}

void TransformSmoother::begin_pending(std::int32_t turn) noexcept {
    pending_ = {};
    pending_.turn = turn;
}

HistoryUpdate TransformSmoother::commit_pending() noexcept {
    const Frame frame{pending_.position, pending_.turn};
    pending_ = {};
    if (count_ != 0 && (!is_next(frames_[count_ - 1].turn, frame.turn) ||
                        discontinuous(frames_[count_ - 1].position, frame.position))) {
        frames_ = {};
        frames_.front() = frame;
        count_ = 1;
        age_ = 1;
        return HistoryUpdate::ResetDiscontinuity;
    }

    if (count_ < frames_.size()) {
        frames_[count_++] = frame;
    } else {
        std::shift_left(frames_.begin(), frames_.end(), 1);
        frames_.back() = frame;
    }
    if (age_ != (std::numeric_limits<std::uint8_t>::max)()) {
        ++age_;
    }
    return HistoryUpdate::Added;
}

void TransformSmoother::clear_history() noexcept {
    frames_ = {};
    pending_ = {};
    count_ = 0;
    age_ = 0;
}

SmoothingResult TransformSmoother::calculate_correction(float ratio, TranslationCorrection& output) const noexcept {
    if (!std::isfinite(ratio) || ratio < 0.0F || ratio > 1.0F) {
        return SmoothingResult::Invalid;
    }
    if (count_ < frames_.size() || age_ <= frames_.size()) {
        return SmoothingResult::HistoryWarmup;
    }

    const auto& first = frames_[0].position;
    const auto& second = frames_[1].position;
    const auto& third = frames_[2].position;
    const auto& fourth = frames_[3].position;
    output.expected_position = interpolate(third, fourth, ratio);
    const auto start = weighted_average(first, second, third);
    const auto end = weighted_average(second, third, fourth);
    const auto candidate_position = interpolate(start, end, ratio);

    const auto ramp_age = static_cast<float>(age_ - frames_.size() - 1U) + ratio;
    const float ramp = (std::min)(1.0F, ramp_age / static_cast<float>(kRampTurns));
    output.blend_weight = ramp * ramp * (3.0F - 2.0F * ramp);
    for (std::size_t axis = 0; axis < output.translation.size(); ++axis) {
        output.translation[axis] = (candidate_position[axis] - output.expected_position[axis]) * output.blend_weight;
    }
    return output.blend_weight < 1.0F ? SmoothingResult::Ramping : SmoothingResult::Smoothed;
}

constexpr std::uint8_t TransformSmoother::publication_mask(Publication publication) noexcept {
    return publication == Publication::Object ? 1U : 2U;
}

bool TransformSmoother::is_next(std::int32_t previous, std::int32_t current) noexcept {
    return static_cast<std::uint32_t>(current) - static_cast<std::uint32_t>(previous) == 1U;
}

bool TransformSmoother::is_older(std::int32_t current, std::int32_t reference) noexcept {
    const auto difference = static_cast<std::uint32_t>(current) - static_cast<std::uint32_t>(reference);
    return difference > 0x8000'0000U;
}

bool TransformSmoother::read_position(Position& output, const float* matrix) noexcept {
    if (matrix == nullptr) {
        return false;
    }
    for (std::size_t axis = 0; axis < output.size(); ++axis) {
        output[axis] = matrix[12 + axis];
        if (!std::isfinite(output[axis])) {
            return false;
        }
    }
    return true;
}

void TransformSmoother::write_position(const Position& position, float* matrix) noexcept {
    for (std::size_t axis = 0; axis < position.size(); ++axis) {
        matrix[12 + axis] = position[axis];
    }
}

void TransformSmoother::apply_translation(Position& position, const Position& translation) noexcept {
    for (std::size_t axis = 0; axis < position.size(); ++axis) {
        position[axis] += translation[axis];
    }
}

TransformSmoother::Position TransformSmoother::interpolate(const Position& start, const Position& end,
                                                           float ratio) noexcept {
    return {
        std::lerp(start[0], end[0], ratio),
        std::lerp(start[1], end[1], ratio),
        std::lerp(start[2], end[2], ratio),
    };
}

TransformSmoother::Position TransformSmoother::weighted_average(const Position& first, const Position& middle,
                                                                const Position& last) noexcept {
    return {
        (first[0] + 2.0F * middle[0] + last[0]) * 0.25F,
        (first[1] + 2.0F * middle[1] + last[1]) * 0.25F,
        (first[2] + 2.0F * middle[2] + last[2]) * 0.25F,
    };
}

float TransformSmoother::distance_squared(const Position& left, const Position& right) noexcept {
    const float x = left[0] - right[0];
    const float y = left[1] - right[1];
    const float z = left[2] - right[2];
    return x * x + y * y + z * z;
}

bool TransformSmoother::discontinuous(const Position& previous, const Position& current) noexcept {
    return distance_squared(previous, current) > kMaximumStepDistance * kMaximumStepDistance;
}

} // namespace fusioncutter::patches::spectator_camera
