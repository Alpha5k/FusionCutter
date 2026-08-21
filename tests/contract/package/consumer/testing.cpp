#include <FusionCutter/Testing.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_CASE("installed Testing target links through the package") {
    // Construction alone proves the compiled architecture-specific owner is linked; proprietary images are outside
    // what the package contract tests and belong to verifier and slice tests.
    fc::test::Scenario scenario{fc::TargetLayout::GOGRetail, fc::HostRole::Client};
    CHECK(true);
}
