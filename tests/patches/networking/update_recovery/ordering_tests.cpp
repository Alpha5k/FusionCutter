#include "ordering.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <span>

namespace update_recovery = fusioncutter::patches::update_recovery;

namespace {

struct Update {
    std::uint32_t turn;
    char identity;
};

} // namespace

TEST_CASE("Late update recovery orders turns while preserving equal-turn arrival order", "[patches][update_recovery]") {
    std::array updates{
        Update{3, 'a'},
        Update{1, 'b'},
        Update{2, 'c'},
        Update{3, 'd'},
    };

    update_recovery::order_updates_by_turn(std::span{updates});

    CHECK(updates[0].identity == 'b');
    CHECK(updates[1].identity == 'c');
    CHECK(updates[2].identity == 'a');
    CHECK(updates[3].identity == 'd');
}
