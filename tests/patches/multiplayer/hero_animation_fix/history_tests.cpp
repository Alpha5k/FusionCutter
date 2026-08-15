#include "history.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

namespace hero_animation = fusioncutter::patches::hero_animation_fix;

namespace {

[[nodiscard]] hero_animation::HeroIdentity local_identity() noexcept {
    return {.weapon = 1, .owner = 2, .combo = 3, .player_handle = 4, .local_player = 0};
}

[[nodiscard]] hero_animation::HeroIdentity remote_identity() noexcept {
    return {.weapon = 11, .owner = 12, .combo = 13, .player_handle = 14, .local_player = 0xFF};
}

} // namespace

TEST_CASE("Local hero history follows the acknowledged prediction frontier", "[patches][hero_animation]") {
    hero_animation::LocalHistory history;
    const auto identity = local_identity();

    REQUIRE(history.observe_prediction(identity, 0, 2, 100));
    REQUIRE(history.observe_prediction(identity, 2, 6, 100));
    CHECK(history.classify_authority(identity, 6, 0, 99) == hero_animation::AuthorityAction::Historical);
    CHECK(history.classify_authority(identity, 6, 0, 100) == hero_animation::AuthorityAction::Historical);
    CHECK(history.classify_authority(identity, 6, 2, 100) == hero_animation::AuthorityAction::Historical);
    CHECK(history.classify_authority(identity, 6, 6, 100) == hero_animation::AuthorityAction::Native);

    REQUIRE(history.observe_prediction(identity, 6, 0, 120));
    CHECK(history.classify_authority(identity, 0, 6, 119) == hero_animation::AuthorityAction::Historical);
    CHECK(history.classify_authority(identity, 0, 0, 120) == hero_animation::AuthorityAction::Native);
}

TEST_CASE("Local hero history distinguishes repeated occurrences from corrections", "[patches][hero_animation]") {
    hero_animation::LocalHistory history;
    const auto identity = local_identity();

    REQUIRE(history.observe_prediction(identity, 0, 16, 100));
    REQUIRE(history.observe_prediction(identity, 16, 15, 101));
    REQUIRE(history.observe_prediction(identity, 15, 16, 102));

    CHECK(history.classify_authority(identity, 16, 16, 100) == hero_animation::AuthorityAction::Historical);
    CHECK(history.classify_authority(identity, 16, 15, 101) == hero_animation::AuthorityAction::Historical);
    CHECK(history.classify_authority(identity, 16, 16, 102) == hero_animation::AuthorityAction::Native);

    REQUIRE(history.observe_prediction(identity, 16, 0, 110));
    CHECK(history.classify_authority(identity, 0, 7, 110) == hero_animation::AuthorityAction::Corrected);
}

TEST_CASE("Remote hero history bounds idle ambiguity and restores only continuous input", "[patches][hero_animation]") {
    hero_animation::RemoteHistory history;
    const auto identity = remote_identity();

    CHECK(history.classify_authority(identity, 0, 0, 10) == hero_animation::AuthorityAction::Native);
    REQUIRE(history.observe_prediction(identity, 0, 7, 11));
    CHECK(history.classify_authority(identity, 7, 0, 11) == hero_animation::AuthorityAction::Historical);
    CHECK(history.classify_authority(identity, 7, 0, 12) == hero_animation::AuthorityAction::Historical);
    CHECK(history.classify_authority(identity, 7, 0, 13) == hero_animation::AuthorityAction::Corrected);

    std::uint8_t down{};
    history.begin_prediction(identity);
    CHECK_FALSE(history.reconcile_input(identity, 0x22, down));
    history.finish_prediction(identity);

    // Prediction restored an idle authority snapshot, but the public input level remained held.
    down = 0;
    history.begin_prediction(identity);
    REQUIRE(history.reconcile_input(identity, 0x22, down));
    CHECK(down == 0x22);
    history.finish_prediction(identity);

    history.begin_authority(identity);
    history.observe_authority_application(identity, 0, 7);
    history.begin_prediction(identity);
    CHECK_FALSE(history.reconcile_input(identity, 0x26, down));
    CHECK(history.resolve_replay(identity, 7));
    CHECK_FALSE(history.resolve_replay(identity, 7));
    history.finish_prediction(identity);
}
