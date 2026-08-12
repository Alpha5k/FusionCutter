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
                                             float object, float camera) noexcept {
    auto object_matrix = matrix(object);
    auto camera_matrix = matrix(camera);
    static_cast<void>(smoother.publish_object(owner, turn, object_matrix.data()));
    return smoother.publish_camera(owner, turn, camera_matrix.data());
}

} // namespace

TEST_CASE("Spectator smoothing pairs both native paths and ramps in continuously", "[patches][spectator_camera]") {
    spectator_camera::TransformSmoother smoother;
    int owner{};

    CHECK(publish_pair(smoother, &owner, 10, 0.0F, 10.0F) == spectator_camera::HistoryUpdate::Added);
    CHECK(publish_pair(smoother, &owner, 11, 1.0F, 11.0F) == spectator_camera::HistoryUpdate::Added);
    CHECK(publish_pair(smoother, &owner, 12, 2.0F, 12.0F) == spectator_camera::HistoryUpdate::Added);
    CHECK(publish_pair(smoother, &owner, 13, 3.0F, 13.0F) == spectator_camera::HistoryUpdate::Added);
    CHECK_FALSE(smoother.ready(&owner));

    CHECK(publish_pair(smoother, &owner, 14, 4.0F, 14.0F) == spectator_camera::HistoryUpdate::Added);
    REQUIRE(smoother.ready(&owner));

    auto object = matrix(3.5F);
    auto camera = matrix(13.5F);
    CHECK(smoother.smooth(spectator_camera::TransformPath::Object, 0.5F, object.data()) ==
          spectator_camera::SmoothingResult::Ramping);
    CHECK(smoother.smooth(spectator_camera::TransformPath::Camera, 0.5F, camera.data()) ==
          spectator_camera::SmoothingResult::Ramping);
    CHECK(object[12] == Catch::Approx(3.45703125F));
    CHECK(camera[12] == Catch::Approx(13.45703125F));
}

TEST_CASE("Spectator smoothing changes translation without replacing native orientation",
          "[patches][spectator_camera]") {
    spectator_camera::TransformSmoother smoother;
    int owner{};
    for (int turn = 1; turn <= 9; ++turn) {
        REQUIRE(publish_pair(smoother, &owner, turn, static_cast<float>(turn - 1), static_cast<float>(turn + 9)) ==
                spectator_camera::HistoryUpdate::Added);
    }

    auto rendered = matrix(7.5F);
    rendered[0] = 0.0F;
    rendered[2] = 1.0F;
    rendered[8] = -1.0F;
    rendered[10] = 0.0F;
    REQUIRE(smoother.smooth(spectator_camera::TransformPath::Object, 0.5F, rendered.data()) ==
            spectator_camera::SmoothingResult::Smoothed);
    CHECK(rendered[12] == Catch::Approx(6.5F));
    CHECK(rendered[0] == 0.0F);
    CHECK(rendered[2] == 1.0F);
    CHECK(rendered[8] == -1.0F);
    CHECK(rendered[10] == 0.0F);
}

TEST_CASE("Spectator smoothing fails open when native history is no longer coherent", "[patches][spectator_camera]") {
    spectator_camera::TransformSmoother smoother;
    int first_owner{};
    int second_owner{};
    for (int turn = 1; turn <= 5; ++turn) {
        REQUIRE(publish_pair(smoother, &first_owner, turn, static_cast<float>(turn - 1),
                             static_cast<float>(turn + 9)) == spectator_camera::HistoryUpdate::Added);
    }

    auto mismatched = matrix(20.0F);
    CHECK(smoother.smooth(spectator_camera::TransformPath::Object, 0.5F, mismatched.data()) ==
          spectator_camera::SmoothingResult::PhaseMismatch);
    CHECK(mismatched[12] == 20.0F);

    auto gap = matrix(7.0F);
    CHECK(smoother.publish_object(&first_owner, 8, gap.data()) == spectator_camera::HistoryUpdate::ResetGap);
    CHECK(smoother.count() == 0);

    auto next = matrix(8.0F);
    CHECK(smoother.publish_object(&first_owner, 9, next.data()) == spectator_camera::HistoryUpdate::ResetIncomplete);
    CHECK(smoother.count() == 0);

    auto changed_owner = matrix(0.0F);
    CHECK(smoother.publish_camera(&second_owner, 1, changed_owner.data()) ==
          spectator_camera::HistoryUpdate::ResetOwner);
    CHECK(smoother.count() == 0);
}
