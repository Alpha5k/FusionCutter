#include "policy.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>

namespace update_pacing = fusioncutter::patches::update_pacing;

TEST_CASE("Update pacing recognizes supported rates and chooses the nearest 80 Hz release",
          "[patches][update_pacing]") {
    using Clock = std::chrono::steady_clock;

    CHECK(update_pacing::pacing_mode(0.05F) == update_pacing::PacingMode::TwentyTps);
    CHECK(update_pacing::pacing_mode(1.0F / 30.0F) == update_pacing::PacingMode::ThirtyTps);
    CHECK(update_pacing::pacing_mode(1.0F / 60.0F) == update_pacing::PacingMode::Unsupported);

    const Clock::time_point previous{};
    CHECK_FALSE(update_pacing::pacing_decision(previous, previous + std::chrono::microseconds{43'750},
                                               update_pacing::PacingMode::TwentyTps)
                    .hold);
    CHECK(update_pacing::pacing_decision(previous, previous + std::chrono::microseconds{43'749},
                                         update_pacing::PacingMode::TwentyTps)
              .hold);

    const auto held = update_pacing::pacing_decision(previous, previous + std::chrono::microseconds{18'749},
                                                     update_pacing::PacingMode::ThirtyTps);
    CHECK(held.hold);
    CHECK_FALSE(held.cap_limited);

    const auto capped = update_pacing::pacing_decision(previous, previous + std::chrono::milliseconds{10},
                                                       update_pacing::PacingMode::ThirtyTps);
    CHECK(capped.hold);
    CHECK(capped.cap_limited);
}
