#include "smoothing.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>

namespace spectator_camera = fusioncutter::patches::spectator_camera;

namespace {

[[nodiscard]] std::array<float, 16> matrix(float x, float y = 0.0F, float z = 0.0F) noexcept {
    return {
        1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, x, y, z, 1.0F,
    };
}

spectator_camera::HistoryUpdate publish_pair(spectator_camera::TransformSmoother& smoother, void* owner, int turn,
                                             float object) noexcept {
    auto object_matrix = matrix(object);
    static_cast<void>(smoother.publish_object(owner, turn, object_matrix.data()));
    return smoother.confirm_camera(owner, turn);
}

} // namespace

TEST_CASE("Spectator smoothing pairs object updates with camera confirmation and ramps in continuously",
          "[patches][spectator_camera]") {
    spectator_camera::TransformSmoother smoother;
    int owner{};

    CHECK(publish_pair(smoother, &owner, 10, 0.0F) == spectator_camera::HistoryUpdate::Added);
    CHECK(publish_pair(smoother, &owner, 11, 1.0F) == spectator_camera::HistoryUpdate::Added);
    CHECK(publish_pair(smoother, &owner, 12, 2.0F) == spectator_camera::HistoryUpdate::Added);
    CHECK(publish_pair(smoother, &owner, 13, 3.0F) == spectator_camera::HistoryUpdate::Added);

    auto warming_up = matrix(2.5F);
    CHECK(smoother.smooth_object(0.5F, warming_up.data()) == spectator_camera::SmoothingResult::HistoryWarmup);

    CHECK(publish_pair(smoother, &owner, 14, 4.0F) == spectator_camera::HistoryUpdate::Added);

    auto object = matrix(3.5F);
    CHECK(smoother.smooth_object(0.5F, object.data()) == spectator_camera::SmoothingResult::Ramping);
    CHECK(object[12] == Catch::Approx(3.45703125F));
}

TEST_CASE("Spectator smoothing changes only object translation", "[patches][spectator_camera]") {
    spectator_camera::TransformSmoother smoother;
    int owner{};
    for (int turn = 1; turn <= 9; ++turn) {
        REQUIRE(publish_pair(smoother, &owner, turn, static_cast<float>(turn - 1)) ==
                spectator_camera::HistoryUpdate::Added);
    }

    auto object = matrix(7.55F);
    REQUIRE(smoother.smooth_object(0.5F, object.data()) == spectator_camera::SmoothingResult::Smoothed);
    CHECK(object[12] == Catch::Approx(6.55F));
    CHECK(object[0] == 1.0F);
    CHECK(object[5] == 1.0F);
    CHECK(object[10] == 1.0F);
}

TEST_CASE("Spectator smoothing fails open when native history is no longer coherent", "[patches][spectator_camera]") {
    spectator_camera::TransformSmoother smoother;
    int first_owner{};
    int second_owner{};
    for (int turn = 1; turn <= 5; ++turn) {
        REQUIRE(publish_pair(smoother, &first_owner, turn, static_cast<float>(turn - 1)) ==
                spectator_camera::HistoryUpdate::Added);
    }

    auto mismatched = matrix(20.0F);
    CHECK(smoother.smooth_object(0.5F, mismatched.data()) == spectator_camera::SmoothingResult::PhaseMismatch);
    CHECK(mismatched[12] == 20.0F);

    auto gap = matrix(7.0F);
    CHECK(smoother.publish_object(&first_owner, 8, gap.data()) == spectator_camera::HistoryUpdate::ResetGap);

    auto next = matrix(8.0F);
    CHECK(smoother.publish_object(&first_owner, 9, next.data()) == spectator_camera::HistoryUpdate::ResetIncomplete);

    CHECK(smoother.confirm_camera(&second_owner, 1) == spectator_camera::HistoryUpdate::ResetOwner);
}
