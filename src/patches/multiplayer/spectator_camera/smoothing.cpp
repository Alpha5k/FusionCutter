#include "smoothing.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace fusioncutter::patches::spectator_camera {

HistoryUpdate TransformSmoother::publish_object(void* owner, std::int32_t turn, const float* matrix) noexcept {
    return publish(TransformPath::Object, owner, turn, matrix);
}

HistoryUpdate TransformSmoother::publish_camera(void* owner, std::int32_t turn, const float* matrix) noexcept {
    return publish(TransformPath::Camera, owner, turn, matrix);
}

SmoothingResult TransformSmoother::smooth(TransformPath path, float ratio, float* matrix) const noexcept {
    if (matrix == nullptr || !std::isfinite(ratio) || ratio < 0.0F || ratio > 1.0F) {
        return SmoothingResult::Invalid;
    }
    if (count_ < frames_.size() || age_ <= frames_.size()) {
        return SmoothingResult::HistoryWarmup;
    }

    Position native{};
    if (!read_position(native, matrix)) {
        return SmoothingResult::Invalid;
    }

    const auto& first = position(frames_[0], path);
    const auto& second = position(frames_[1], path);
    const auto& third = position(frames_[2], path);
    const auto& fourth = position(frames_[3], path);
    const auto expected = interpolate(third, fourth, ratio);
    if (distance_squared(expected, native) > kPhaseTolerance * kPhaseTolerance) {
        return SmoothingResult::PhaseMismatch;
    }

    const auto start = weighted_average(first, second, third);
    const auto end = weighted_average(second, third, fourth);
    const auto candidate = interpolate(start, end, ratio);
    const auto ramp_age = static_cast<float>(age_ - frames_.size() - 1U) + ratio;
    const float ramp = (std::min)(1.0F, ramp_age / static_cast<float>(kRampTurns));
    const float weight = ramp * ramp * (3.0F - 2.0F * ramp);
    write_position(interpolate(native, candidate, weight), matrix);
    return weight < 1.0F ? SmoothingResult::Ramping : SmoothingResult::Smoothed;
}

void TransformSmoother::reset() noexcept {
    frames_ = {};
    pending_ = {};
    owner_ = nullptr;
    count_ = 0;
    age_ = 0;
}

bool TransformSmoother::ready(void* owner) const noexcept {
    return owner != nullptr && owner_ == owner && count_ == frames_.size() && age_ > frames_.size();
}

std::size_t TransformSmoother::count() const noexcept {
    return count_;
}

HistoryUpdate TransformSmoother::publish(TransformPath path, void* owner, std::int32_t turn,
                                         const float* matrix) noexcept {
    Position sample{};
    if (owner == nullptr || turn < 0 || !read_position(sample, matrix)) {
        return HistoryUpdate::Invalid;
    }

    auto result = HistoryUpdate::Pending;
    if (owner_ != owner) {
        reset();
        owner_ = owner;
        result = HistoryUpdate::ResetOwner;
    }

    if (count_ != 0 && turn == frames_[count_ - 1].turn) {
        position(frames_[count_ - 1], path) = sample;
        if (count_ > 1 && discontinuous(position(frames_[count_ - 2], path), sample)) {
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

    if (pending_.paths != 0) {
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

    const auto mask = path_mask(path);
    const bool replaced = (pending_.paths & mask) != 0;
    position(pending_, path) = sample;
    pending_.paths = static_cast<std::uint8_t>(pending_.paths | mask);
    if (pending_.paths != 3U) {
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
    const Frame frame{pending_.object, pending_.camera, pending_.turn};
    pending_ = {};
    if (count_ != 0 &&
        (!is_next(frames_[count_ - 1].turn, frame.turn) || discontinuous(frames_[count_ - 1].object, frame.object) ||
         discontinuous(frames_[count_ - 1].camera, frame.camera))) {
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

constexpr std::uint8_t TransformSmoother::path_mask(TransformPath path) noexcept {
    return path == TransformPath::Object ? 1U : 2U;
}

TransformSmoother::Position& TransformSmoother::position(Frame& frame, TransformPath path) noexcept {
    return path == TransformPath::Object ? frame.object : frame.camera;
}

const TransformSmoother::Position& TransformSmoother::position(const Frame& frame, TransformPath path) noexcept {
    return path == TransformPath::Object ? frame.object : frame.camera;
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
