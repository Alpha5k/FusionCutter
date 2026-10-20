#include "fixture.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <string_view>

namespace {

using fc::lua::Frame;
using fc::lua::Type;
using fc::tests::lua::Fixture;

} // namespace

TEST_CASE("Lua frames use strict conversions and preserve counted strings and tagged userdata", "[lua][frame]") {
    Fixture fixture;
    bool conversions = false;
    const std::array payload{std::byte{1}, std::byte{0}, std::byte{255}};

    REQUIRE(fixture.bindings->bind("Inspect", [&](Frame &frame) noexcept {
        const auto boolean = frame.argument(0).as_bool();
        const auto number = frame.argument(1).as_number();
        const auto text = frame.argument(2).as_string();
        const auto data = frame.userdata("test.bytes", payload);
        const auto read_data = data.as_userdata("test.bytes");

        conversions = boolean && *boolean && number && *number == 42 && text && *text == std::string_view{"a\0b", 3} &&
                      !frame.argument(1).as_string() && data.type() == Type::Userdata && read_data &&
                      std::ranges::equal(*read_data, payload) && !data.as_userdata("other.tag");

        frame.return_values({frame.value(std::string_view{"x\0y", 3}), frame.value(42), frame.value("literal"), frame.nil()});
    }));
    fixture.start();

    fixture.script(R"(
        local text, number, literal, missing = Inspect(true, 42, "a\000b")
        assert(text == "x\000y" and number == 42 and literal == "literal" and missing == nil)
    )");
    CHECK(conversions);
    CHECK(lua_gettop(fixture.vm.get()) == 0);
}

TEST_CASE("Lua errors preserve C++ cleanup and restore the invocation stack", "[lua][frame]") {
    Fixture fixture;
    unsigned cleanups = 0;
    bool call_rejected = false;
    bool reference_rejected = false;

    // Destruction must occur before the C trampoline raises a pending Lua error.
    struct Cleanup {
        unsigned &count;

        ~Cleanup() {
            ++count;
        }
    };

    REQUIRE(fixture.bindings->bind("AccessFailure", [&](Frame &frame) noexcept {
        Cleanup cleanup{cleanups};
        const auto function = frame.global("ShouldNotRun");
        auto retained = frame.retain(function);
        frame.global("Broken").get("key");
        call_rejected = !frame.result() && !frame.call(function, {});
        reference_rejected = retained && !frame.call(*retained, {});
        frame.return_values({frame.value(true)});
    }));
    REQUIRE(fixture.bindings->bind("DeclaredFailure", [&](Frame &frame) noexcept {
        Cleanup cleanup{cleanups};
        frame.error("deliberate callback failure");
    }));
    REQUIRE(fixture.bindings->bind("HandledFailure", [&](Frame &frame) noexcept {
        Cleanup cleanup{cleanups};
        auto failed = frame.execute("error('handled script failure')");
        auto result = frame.execute("return 12, 'done'");
        if (!failed && result && result->size() == 2 && frame.result()) {
            frame.return_values(result->values());
        } else {
            frame.error("Lua execution did not return the expected results");
        }
    }));
    fixture.start();

    fixture.script(R"(
        SideEffects = 0
        ShouldNotRun = function() SideEffects = SideEffects + 1 end
        Broken = setmetatable({}, {__index = function() error("table access failed") end})
        local ok, reason = pcall(AccessFailure)
        assert(not ok and string.find(reason, "table access failed", 1, true))
        ok, reason = pcall(DeclaredFailure)
        assert(not ok and reason == "deliberate callback failure")
        local number, text = HandledFailure()
        assert(number == 12 and text == "done" and SideEffects == 0)
    )");
    CHECK(cleanups == 3);
    CHECK(call_rejected);
    CHECK(reference_rejected);
    CHECK(lua_gettop(fixture.vm.get()) == 0);
}

TEST_CASE("Lua result selection distinguishes zero results and nil and survives later pushes", "[lua][frame]") {
    Fixture fixture;
    REQUIRE(fixture.bindings->bind("NoResults", [](Frame &) noexcept {}));
    REQUIRE(fixture.bindings->bind("OneNil", [](Frame &frame) noexcept {
        frame.return_values({frame.nil()});
    }));
    REQUIRE(fixture.bindings->bind("EarlierResults", [](Frame &frame) noexcept {
        frame.return_values({frame.value(12), frame.nil()});
        frame.value("later");
    }));
    fixture.start();
    fixture.script(R"(
        local function count(...) return arg.n end
        assert(count(NoResults()) == 0)
        assert(count(OneNil()) == 1 and OneNil() == nil)
        local first, second = EarlierResults()
        assert(count(EarlierResults()) == 2 and first == 12 and second == nil)
    )");
    CHECK(lua_gettop(fixture.vm.get()) == 0);
}
