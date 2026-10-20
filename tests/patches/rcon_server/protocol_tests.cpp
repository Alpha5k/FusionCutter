#include "gog/authentication.hpp"
#include "gog/protocol.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <span>
#include <string>
#include <vector>

namespace protocol = fc::core_plugin::rcon_server::gog::protocol;

TEST_CASE("RCON request framing distinguishes partial, complete, and malformed input") {
    // One row with length eight contains the seven-character /status command and its terminator.
    constexpr std::array request = {'\x01', '\x08', '/', 's', 't', 'a', 't', 'u', 's', '\0'};

    CHECK(protocol::decode_request(std::span{request}.first(request.size() - 1)).state == protocol::FrameState::Incomplete);

    const auto complete = protocol::decode_request(request);

    REQUIRE(complete.state == protocol::FrameState::Complete);
    CHECK(std::string{complete.command} == "/status");
    CHECK(complete.consumed == request.size());

    auto malformed = request;
    malformed[0] = '\x02';
    CHECK(protocol::decode_request(malformed).state == protocol::FrameState::Invalid);

    malformed = request;
    malformed[5] = '\0';
    CHECK(protocol::decode_request(malformed).state == protocol::FrameState::Invalid);
}

TEST_CASE("RCON responses preserve rows and replace oversized output with the protocol error") {
    // Mixed line endings and a trailing separator exercise the exact row layout consumed by administration tools.
    const auto response = protocol::encode_message("first\r\nsecond\n");
    const std::array expected = {'\x02', '\x06', 'f', 'i', 'r', 's', 't', '\0', '\x07', 's', 'e', 'c', 'o', 'n', 'd', '\0'};

    CHECK(response == std::vector<char>{expected.begin(), expected.end()});

    // The one-byte row length includes its terminator, so 255 content bytes require the replacement packet.
    const std::string oversized(0xff, 'x');
    const auto limited_response = protocol::encode_message(oversized);
    const auto limited = protocol::decode_request(limited_response);

    REQUIRE(limited.state == protocol::FrameState::Complete);
    CHECK(std::string{limited.command} == "RCON response exceeds protocol limit");
}

TEST_CASE("RCON authentication accepts the legacy lowercase MD5 password digest") {
    fc::core_plugin::rcon_server::gog::Authentication authentication;
    REQUIRE(authentication.prepare());

    CHECK(authentication.matches("password", "5f4dcc3b5aa765d61d8327deb882cf99"));
    CHECK_FALSE(authentication.matches("password", "5F4DCC3B5AA765D61D8327DEB882CF99"));
    CHECK_FALSE(authentication.matches("different", "5f4dcc3b5aa765d61d8327deb882cf99"));
}
