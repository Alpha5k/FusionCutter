#include "history.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

namespace hero_animation = fusioncutter::patches::hero_animation_fix;

namespace {

[[nodiscard]] hero_animation::HeroIdentity local_identity(std::uintptr_t combo = 3) noexcept {
    return {.weapon = 1, .owner = 2, .combo = combo, .player_handle = 4, .local_player = 0};
}

[[nodiscard]] hero_animation::HeroIdentity remote_identity(std::uintptr_t combo = 13) noexcept {
    return {.weapon = 11, .owner = 12, .combo = combo, .player_handle = 14, .local_player = 0xFF};
}

} // namespace

TEST_CASE("Local hero history rejects acknowledged replay without delaying corrections", "[patches][hero_animation]") {
    hero_animation::HeroHistory history;
    const auto identity = local_identity();

    REQUIRE(history.observe_prediction(identity, 0, 7, 100));
    REQUIRE(history.observe_prediction(identity, 7, 8, 101));
    CHECK(history.classify_authority(identity, 8, 7, 99) == hero_animation::AuthorityAction::SuppressHistorical);
    CHECK(history.classify_authority(identity, 8, 0, 100) == hero_animation::AuthorityAction::SuppressHistorical);
    CHECK(history.should_suppress_local_presentation(identity, 8, 100));

    CHECK(history.classify_authority(identity, 8, 0, 101) == hero_animation::AuthorityAction::Apply);
    CHECK_FALSE(history.local_action_active(identity));
}

TEST_CASE("Remote hero history preserves repeated states and applies ambiguous authority",
          "[patches][hero_animation]") {
    hero_animation::HeroHistory history;
    const auto identity = remote_identity();

    REQUIRE(history.observe_prediction(identity, 0, 7, 0));
    REQUIRE(history.observe_prediction(identity, 7, 8, 0));
    REQUIRE(history.observe_prediction(identity, 8, 7, 0));

    CHECK(history.classify_authority(identity, 7, 7, 0) == hero_animation::AuthorityAction::Apply);
    CHECK(history.classify_authority(identity, 7, 8, 0) == hero_animation::AuthorityAction::SuppressHistorical);
    CHECK(history.classify_authority(identity, 7, 7, 0) == hero_animation::AuthorityAction::Apply);

    REQUIRE(history.observe_prediction(identity, 7, 9, 0));
    CHECK(history.classify_authority(identity, 9, 7, 0) == hero_animation::AuthorityAction::Apply);
}

TEST_CASE("Hero reconciliation is one-shot, identity-bound, and bounded", "[patches][hero_animation]") {
    hero_animation::HeroHistory history;
    const auto identity = remote_identity();

    history.record_authority_transition(identity, 7);
    std::uint8_t down = 0x80;
    REQUIRE(history.reconcile_input(identity, 0x26, down));
    CHECK(down == 0xA6);
    CHECK_FALSE(history.reconcile_input(identity, 0x26, down));
    CHECK(history.resolve_replay(identity, 7));

    history.record_authority_transition(identity, 8);
    CHECK_FALSE(history.resolve_replay(identity, 7));
    CHECK_FALSE(history.resolve_replay(identity, 8));

    history.record_authority_transition(identity, 9);
    const auto replacement = remote_identity(99);
    history.begin_prediction(replacement);
    down = 0;
    CHECK_FALSE(history.reconcile_input(identity, 0x02, down));
    CHECK_FALSE(history.resolve_replay(identity, 9));

    auto base_state = 0;
    for (std::size_t index = 0; index < hero_animation::kRemoteStateCapacity; ++index) {
        const auto target_state = base_state == 1 ? 2 : 1;
        REQUIRE(history.observe_prediction(replacement, base_state, target_state, 0));
        base_state = target_state;
    }
    const auto overflow_target = base_state == 1 ? 2 : 1;
    CHECK_FALSE(history.observe_prediction(replacement, base_state, overflow_target, 0));
}
