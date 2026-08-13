#include "../../../../src/patches/networking/network_diagnostics/trace/ring.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

namespace trace = fusioncutter::patches::network_diagnostics::trace;

TEST_CASE("Network Diagnostics preserves bounded records across ring wrap") {
    trace::SpscRing<std::uint32_t, 16> records;

    for (std::uint32_t value = 1; value <= 15; ++value) {
        CHECK(records.push(value));
    }
    CHECK_FALSE(records.push(16));
    CHECK(records.size() == 15);

    std::uint32_t record{};
    REQUIRE(records.pop(record));
    CHECK(record == 1);
    CHECK(records.push(16));

    for (std::uint32_t expected = 2; expected <= 16; ++expected) {
        REQUIRE(records.pop(record));
        CHECK(record == expected);
    }
    CHECK_FALSE(records.pop(record));
}
