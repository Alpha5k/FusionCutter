#pragma once

#include <chrono>
#include <cmath>

namespace fusioncutter::patches::update_pacing {

enum class PacingMode {
    Unsupported,
    TwentyTps,
    ThirtyTps,
};

inline constexpr auto kNetworkServiceInterval = std::chrono::microseconds{12'500};

[[nodiscard]] inline PacingMode pacing_mode(float fixed_delta) noexcept {
    constexpr float kTolerance = 0.0005F;
    if (std::abs(fixed_delta - 0.05F) <= kTolerance) {
        return PacingMode::TwentyTps;
    }
    if (std::abs(fixed_delta - (1.0F / 30.0F)) <= kTolerance) {
        return PacingMode::ThirtyTps;
    }
    return PacingMode::Unsupported;
}

[[nodiscard]] inline std::chrono::steady_clock::duration target_spacing(PacingMode mode) noexcept {
    switch (mode) {
    case PacingMode::TwentyTps:
        return kNetworkServiceInterval * 4;
    case PacingMode::ThirtyTps:
        return kNetworkServiceInterval * 2;
    case PacingMode::Unsupported:
        return {};
    }
    return {};
}

struct PacingDecision {
    bool hold{};
    bool cap_limited{};
};

// Choose whichever 80 Hz service opportunity is closer to the target release time.
[[nodiscard]] inline PacingDecision pacing_decision(std::chrono::steady_clock::time_point previous_release,
                                                    std::chrono::steady_clock::time_point completion,
                                                    PacingMode mode) noexcept {
    const auto correction = previous_release + target_spacing(mode) - completion;
    return {
        .hold = correction > kNetworkServiceInterval / 2,
        .cap_limited = correction > kNetworkServiceInterval,
    };
}

} // namespace fusioncutter::patches::update_pacing
