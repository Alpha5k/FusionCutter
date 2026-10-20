#include "../../../patches/lua_multiplayer/protocol.hpp"

#include "../lua_bindings/fixture.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>
#include <string_view>

namespace {
namespace protocol = fc::core_plugin::lua_multiplayer::protocol;
using fc::lua::Frame;
using fc::tests::lua::Fixture;
} // namespace

TEST_CASE("Lua multiplayer packets preserve listeners and plain values and reject malformed data", "[lua][multiplayer][protocol]") {
    const std::array listeners{protocol::Listener{17, "example.Score"}};
    auto announcement = protocol::encode_listeners(listeners, 2048);
    REQUIRE(announcement);
    auto decoded_listeners = protocol::decode_listeners(*announcement);
    REQUIRE(decoded_listeners);
    CHECK(decoded_listeners->at("example.Score") == 17);
    CHECK_FALSE(protocol::decode_listeners(std::span{*announcement}.first(announcement->size() - 1)));

    Fixture fixture;
    fixture.start();
    fixture.script(R"(CodecValue = {[true] = false, [7] = {text = 'a\000b'}}; Cycle = {}; Cycle.self = Cycle)");
    REQUIRE(fixture.bindings->run(fixture.state, [&](Frame &frame) {
        auto encoded = protocol::encode_value(frame.global("CodecValue"), 2048 - protocol::kMessageHeaderBytes);
        REQUIRE(encoded);
        auto packet = protocol::encode_message(17, *encoded, 2048);
        REQUIRE(packet);
        auto message = protocol::decode_message(*packet);
        REQUIRE(message);
        CHECK(message->destination == 17);
        CHECK(std::ranges::equal(message->value, *encoded));
        auto decoded = protocol::decode_value(frame, message->value);
        REQUIRE(decoded);
        CHECK(decoded->get(true).as_bool().value() == false);
        CHECK(decoded->get(7).get("text").as_string().value() == std::string_view{"a\0b", 3});

        auto too_large = protocol::encode_value(frame.global("CodecValue"), encoded->size() - 1);
        REQUIRE_FALSE(too_large);
        CHECK(too_large.error() == protocol::EncodeError::TooLarge);
        auto cycle = protocol::encode_value(frame.global("Cycle"), 2048);
        REQUIRE_FALSE(cycle);
        CHECK(cycle.error() == protocol::EncodeError::InvalidValue);
        CHECK_FALSE(protocol::decode_value(frame, std::span{*encoded}.first(encoded->size() - 1)));
        const std::array unknown{std::byte{255}};
        CHECK_FALSE(protocol::decode_value(frame, unknown));
    }));
}
