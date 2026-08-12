#include "packed_ledger.hpp"

#include <catch2/catch_test_macros.hpp>

namespace weapon_swap = fusioncutter::patches::weapon_swap_replay;

TEST_CASE("Weapon swap requests distinguish prediction replay from a new transition", "[patches][weapon_swap]") {
    int soldier{};
    int weapons[3]{};
    weapon_swap::LocalSwapLedger ledger;

    const auto first = ledger.record_local(&soldier, 0, 0, 0, 1, &weapons[0], &weapons[1], 100, {95, 100, 95});
    REQUIRE(first.kind == weapon_swap::RequestKind::New);
    REQUIRE(first.epoch != 0);

    const auto replay = ledger.record_local(&soldier, 0, 0, 0, 1, &weapons[0], &weapons[1], 100, {96, 100, 96});
    CHECK(replay.kind == weapon_swap::RequestKind::Replay);
    CHECK(replay.epoch == first.epoch);

    const auto next = ledger.record_local(&soldier, 0, 0, 1, 2, &weapons[1], &weapons[2], 101, {97, 101, 97});
    CHECK(next.kind == weapon_swap::RequestKind::New);
    weapon_swap::EpochSnapshot snapshot;
    REQUIRE(ledger.find(&soldier, 0, 0, snapshot));
    CHECK(snapshot.final_index == 2);

    ledger.observe_frontiers({105, 105, 97});
    const auto historical = ledger.record_local(&soldier, 0, 0, 1, 2, &weapons[1], &weapons[2], 101, {101, 101, 97});
    CHECK(historical.kind == weapon_swap::RequestKind::Replay);
}

TEST_CASE("Weapon swap reconciliation keeps two replay turns before accepting authority", "[patches][weapon_swap]") {
    int soldier{};
    int weapons[3]{};
    weapon_swap::LocalSwapLedger ledger;
    REQUIRE(ledger.record_local(&soldier, 0, 0, 1, 2, &weapons[1], &weapons[2], 100, {95, 100, 95}).valid());

    auto decision = ledger.observe_authoritative(&soldier, 0, 0, 1, 2, &weapons[1], &weapons[2], {110, 110, 100});
    CHECK(decision.kind == weapon_swap::AuthoritativeKind::Pending);
    CHECK(decision.mute_select);

    decision = ledger.observe_authoritative(&soldier, 0, 0, 1, 2, &weapons[1], &weapons[2], {112, 112, 100});
    CHECK(decision.kind == weapon_swap::AuthoritativeKind::Pending);
    CHECK(ledger.has_active());

    decision = ledger.observe_authoritative(&soldier, 0, 0, 1, 2, &weapons[1], &weapons[2], {113, 113, 100});
    CHECK(decision.kind == weapon_swap::AuthoritativeKind::Accepted);
    CHECK_FALSE(ledger.has_active());
}

TEST_CASE("Weapon swap reconciliation fails open when inventory identity changes", "[patches][weapon_swap]") {
    int soldier{};
    int weapons[4]{};
    weapon_swap::LocalSwapLedger ledger;
    REQUIRE(ledger.record_local(&soldier, 0, 0, 1, 2, &weapons[1], &weapons[2], 200, {190, 200, 190}).valid());

    const auto decision = ledger.observe_authoritative(&soldier, 0, 0, 2, 1, &weapons[2], &weapons[3], {195, 200, 195});
    CHECK(decision.kind == weapon_swap::AuthoritativeKind::Correction);
    CHECK(decision.pointer_invalidated);
    CHECK_FALSE(decision.mute_select);
    CHECK_FALSE(ledger.has_active());
}

TEST_CASE("Packed weapon ownership survives channel acceptance until packed authority catches up",
          "[patches][weapon_swap]") {
    int soldier{};
    int weapons[3]{};
    weapon_swap::PackedSwapLedger ledger;
    REQUIRE(ledger.track_projected(&soldier, 0, 1, 2, &weapons[2], 300, 7, 8, {299, 300, 299}));
    const auto replay = ledger.observe_transition(&soldier, 0, 1, 2, &weapons[2], 0, 0, &weapons[0], {301, 301, 299});
    REQUIRE(replay.mute_select);

    ledger.observe_channel_result(0, {weapon_swap::AuthoritativeKind::Accepted, 7, 8, true, false});
    REQUIRE(ledger.has_active());

    weapon_swap::PackedSnapshot snapshot;
    REQUIRE(ledger.resolve(&soldier, 0, 0, 0, &weapons[0], snapshot));
    CHECK(snapshot.projected_key == weapon_swap::PackedSwapLedger::key(1, 2));
    CHECK(weapon_swap::PackedSwapLedger::project_selection(0xC0, snapshot.projected_key) == 0xD2);
}
