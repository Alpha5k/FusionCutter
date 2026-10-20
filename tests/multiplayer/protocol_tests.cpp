#include "message_protocol.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace protocol = fc::runtime::message_protocol;

namespace {

constexpr protocol::Tokens kTokens{0x0807060504030201ULL, 0x1817161514131211ULL};

// Builds expected packet bytes without using the protocol encoder.
std::vector<std::byte> bytes(std::initializer_list<unsigned int> octets) {
    std::vector<std::byte> result;
    for (const auto value : octets) {
        result.push_back(static_cast<std::byte>(value));
    }
    return result;
}

// Returns the encoded portion of the buffer for parsing or comparison.
std::span<const std::byte> frame(const std::array<std::byte, protocol::kFrameBytes> &buffer, std::size_t size) {
    return std::span(buffer).first(size);
}

} // namespace

TEST_CASE("Protocol writes the FCM1 header and little-endian handshake tokens", "[multiplayer][protocol]") {
    const auto ready = bytes({'F', 'C', 'M', '1', 5, 1, 2, 3, 4, 5, 6, 7, 8, 17, 18, 19, 20, 21, 22, 23, 24});
    std::array<std::byte, protocol::kFrameBytes> buffer{};
    const auto size = protocol::encode_ready(kTokens, buffer);
    REQUIRE(std::ranges::equal(frame(buffer, size), ready));

    const auto parsed = protocol::parse(ready);
    REQUIRE(parsed);
    CHECK(parsed->kind == protocol::Kind::Ready);
    CHECK(parsed->tokens.client == kTokens.client);
    CHECK(parsed->tokens.host == kTokens.host);

    const auto hello = bytes({'F', 'C', 'M', '1', 1, 1, 2, 3, 4, 5, 6, 7, 8, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 3, 0, 'a', 'b', 'c'});
    const std::array<std::string_view, 1> manifest{"abc"};
    const auto hello_size = protocol::encode_manifest(protocol::Kind::Hello, {kTokens.client, 0}, manifest, buffer);
    REQUIRE(std::ranges::equal(frame(buffer, hello_size), hello));

    const auto parsed_hello = protocol::parse(hello);
    REQUIRE(parsed_hello);
    CHECK(parsed_hello->manifest == std::vector<std::string_view>{"abc"});
}

TEST_CASE("Protocol carries exact feature IDs, binary payloads and failure notices", "[multiplayer][protocol]") {
    std::array<std::byte, protocol::kFrameBytes> buffer{};
    const std::string feature{"chat\0\xc3\xa9", 7};
    const auto payload = bytes({0, 255, 42});
    const auto size = protocol::encode_data(kTokens, feature, payload, buffer);
    const auto parsed = protocol::parse(frame(buffer, size));
    REQUIRE(parsed);
    CHECK(parsed->kind == protocol::Kind::Data);
    CHECK(parsed->feature == feature);
    CHECK(std::ranges::equal(parsed->payload, payload));

    const std::array manifest{std::string_view{feature}};
    const auto manifest_size = protocol::encode_manifest(protocol::Kind::Welcome, kTokens, manifest, buffer);
    const auto welcome = protocol::parse(frame(buffer, manifest_size));
    REQUIRE(welcome);
    CHECK(std::ranges::equal(welcome->manifest, manifest));

    for (const auto id : {std::string_view{feature}, std::string_view{}}) {
        const auto failed_size = protocol::encode_failed(kTokens, id, buffer);
        const auto failed = protocol::parse(frame(buffer, failed_size));
        REQUIRE(failed);
        CHECK(failed->kind == protocol::Kind::Failed);
        CHECK(failed->feature == id);
    }
}

TEST_CASE("Protocol rejects incomplete frames, invalid tokens and malformed feature IDs", "[multiplayer][protocol]") {
    std::array<std::byte, protocol::kFrameBytes> buffer{};
    const auto payload = bytes({1, 2, 3});
    const auto size = protocol::encode_data(kTokens, "id", payload, buffer);
    CHECK_FALSE(protocol::parse(frame(buffer, size - 1)));
    CHECK_FALSE(protocol::parse(frame(buffer, size + 1)));
    buffer[3] = std::byte{'2'};
    CHECK_FALSE(protocol::parse(frame(buffer, size)));

    const auto ready_size = protocol::encode_ready(kTokens, buffer);
    std::fill(buffer.begin() + 5, buffer.begin() + 13, std::byte{});
    CHECK_FALSE(protocol::parse(frame(buffer, ready_size)));

    const std::array<std::string_view, 2> duplicate{"aa", "aa"};
    CHECK(protocol::encode_manifest(protocol::Kind::Welcome, kTokens, duplicate, buffer) == 0);
    const std::array<std::string_view, 2> valid{"aa", "bb"};
    const auto manifest_size = protocol::encode_manifest(protocol::Kind::Welcome, kTokens, valid, buffer);
    // Make the second ID duplicate the first without changing the encoded lengths.
    buffer[29] = buffer[30] = std::byte{'a'};
    CHECK_FALSE(protocol::parse(frame(buffer, manifest_size)));

    CHECK_FALSE(protocol::valid_feature_id("\xc0\xaf"));
    const auto data_size = protocol::encode_data(kTokens, "a", {}, buffer);
    buffer[25] = std::byte{255};
    CHECK_FALSE(protocol::parse(frame(buffer, data_size)));
}
