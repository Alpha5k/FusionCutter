#include "gog/protocol.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <span>
#include <string>
#include <vector>

namespace protocol = fusioncutter::patches::rcon_server::gog::protocol;

TEST_CASE("RCON request framing handles partial and malformed input") {
    constexpr std::array request = {'\x01', '\x08', '/', 's', 't', 'a', 't', 'u', 's', '\0'};

    CHECK(protocol::decode_request(std::span{request}.first(request.size() - 1)).state ==
          protocol::FrameState::Incomplete);

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

TEST_CASE("RCON response framing preserves rows and enforces wire limits") {
    const auto response = protocol::encode_message("first\r\nsecond\n");
    const std::array expected = {'\x02', '\x06', 'f', 'i', 'r', 's', 't', '\0',
                                 '\x07', 's',    'e', 'c', 'o', 'n', 'd', '\0'};
    const std::vector<char> expected_response{expected.begin(), expected.end()};
    CHECK(response == expected_response);

    const std::string oversized(0xFF, 'x');
    const auto limited_response = protocol::encode_message(oversized);
    const auto limited = protocol::decode_request(limited_response);
    REQUIRE(limited.state == protocol::FrameState::Complete);
    CHECK(std::string{limited.command} == "RCON response exceeds protocol limit");
}
