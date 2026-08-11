#include "shared/datagram.hpp"
#include "shared/protocol.hpp"
#include "shared/security.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

namespace {

using namespace fusioncutter::patches::direct_transport;

TEST_CASE("Direct Transport preserves the deployed control and datagram wire format", "[patches][direct_transport]") {
    std::array<std::uint8_t, kCapsBytes> caps{};
    REQUIRE(write_caps(caps, 0x0123456789ABCDEFULL));
    constexpr std::array<std::uint8_t, kCapsBytes> expected_caps{
        'B', 'F', '2', 'C', 1, 1, 0, 16, 0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF,
    };
    CHECK(caps == expected_caps);

    ParsedControl control{};
    REQUIRE(parse_control(caps, control) == ParseStatus::Ok);
    CHECK(control.kind == ControlKind::Caps);
    CHECK(control.client_nonce == 0x0123456789ABCDEFULL);

    SessionKey key{};
    for (std::uint8_t index = 0; index < key.size(); ++index) {
        key[index] = index;
    }
    std::array<std::uint8_t, kOfferBytes> offer{};
    REQUIRE(write_offer(offer, 0x1122334455667788ULL, 0x99AABBCC, 0x04030201, 3658, key));
    constexpr std::array<std::uint8_t, kOfferBytes> expected_offer{
        'B', 'F', '2', 'C',  1,    2, 0, 42, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xAA, 0xBB, 0xCC, 1,
        2,   3,   4,   0x0E, 0x4A, 0, 1, 2,  3,    4,    5,    6,    7,    8,    9,    10,   11,   12,   13,   14,   15,
    };
    CHECK(offer == expected_offer);
    REQUIRE(parse_control(offer, control) == ParseStatus::Ok);
    CHECK(control.kind == ControlKind::Offer);
    CHECK(control.client_nonce == 0x1122334455667788ULL);
    CHECK(control.connection_id == 0x99AABBCC);
    CHECK(control.server_ipv4_network_order == 0x04030201);
    CHECK(control.server_port == 3658);
    CHECK(control.session_key == key);

    std::array<std::uint8_t, kActivationBytes> activation{};
    REQUIRE(write_activation(activation, ControlKind::Ready, 0xDEADBEEF));
    REQUIRE(parse_control(activation, control) == ParseStatus::Ok);
    CHECK(control.kind == ControlKind::Ready);
    CHECK(control.connection_id == 0xDEADBEEF);

    const std::array<std::uint8_t, 5> payload{0x0B, 1, 2, 3, 4};
    std::array<std::uint8_t, kMaximumDirectDatagramBytes> datagram{};
    REQUIRE(write_direct_header(datagram, DirectKind::Data, 0x12345678, 0x90ABCDEF, 0x0102030405060708ULL));
    constexpr std::array<std::uint8_t, kDirectHeaderBytes> expected_header{
        'B', 'F', '2', 'D', 1, 1, 0, 24, 0x12, 0x34, 0x56, 0x78, 0x90, 0xAB, 0xCD, 0xEF, 1, 2, 3, 4, 5, 6, 7, 8,
    };
    CHECK(std::ranges::equal(expected_header, std::span(datagram).first(kDirectHeaderBytes)));

    const auto data_bytes = write_data_datagram(datagram, Direction::ClientToServer, key, 0x12345678, 9, payload);
    REQUIRE(data_bytes == kDirectHeaderBytes + payload.size());
    const auto data = std::span<const std::uint8_t>(datagram).first(data_bytes);
    ParsedDirect direct{};
    REQUIRE(parse_direct(data, direct) == ParseStatus::Ok);
    CHECK(direct.kind == DirectKind::Data);
    CHECK(direct.connection_id == 0x12345678);
    CHECK(direct.datagram_sequence == 9);
    CHECK(std::ranges::equal(direct.payload, payload));
    CHECK(verify_authentication_tag(Direction::ClientToServer, key, data.first(kDirectHeaderBytes), direct.payload));

    REQUIRE(write_probe_datagram(datagram, DirectKind::ProbeAck, Direction::ServerToClient, key, 7, 11));
    const auto probe = std::span<const std::uint8_t>(datagram).first(kProbeDatagramBytes);
    REQUIRE(parse_direct(probe, direct) == ParseStatus::Ok);
    CHECK(direct.kind == DirectKind::ProbeAck);
    CHECK(verify_authentication_tag(Direction::ServerToClient, key, probe.first(kDirectHeaderBytes), direct.payload));
}

TEST_CASE("Direct Transport rejects malformed framing at its protocol boundaries", "[patches][direct_transport]") {
    ParsedControl control{};
    std::array<std::uint8_t, kCapsBytes> caps{};
    REQUIRE(write_caps(caps, 1));

    auto invalid_caps = caps;
    invalid_caps[0] = 0;
    CHECK(parse_control(invalid_caps, control) == ParseStatus::BadMagic);
    invalid_caps = caps;
    invalid_caps[4] = 2;
    CHECK(parse_control(invalid_caps, control) == ParseStatus::UnsupportedVersion);
    invalid_caps = caps;
    invalid_caps[7] = 15;
    CHECK(parse_control(invalid_caps, control) == ParseStatus::BadLength);

    SessionKey key{};
    std::array<std::uint8_t, kMaximumDirectDatagramBytes + 1> datagram{};
    const std::array<std::uint8_t, kNativeHeaderBytes - 1> short_payload{};
    CHECK(write_data_datagram(datagram, Direction::ClientToServer, key, 1, 1, short_payload) == 0);
    UdpSocketRuntime socket;
    const auto transmit = send_direct_data(socket, datagram, Direction::ClientToServer, key, 1, 1,
                                           Endpoint{0x0100007F, 3658}, short_payload);
    CHECK(transmit.failure == NativeTransmitResult::Failure::Serialization);
    const std::array<std::uint8_t, kMaximumNativeBytes + 1> long_payload{};
    CHECK(write_data_datagram(datagram, Direction::ClientToServer, key, 1, 1, long_payload) == 0);
    CHECK_FALSE(write_probe_datagram(datagram, DirectKind::Data, Direction::ClientToServer, key, 1, 1));

    REQUIRE(write_probe_datagram(datagram, DirectKind::Probe, Direction::ClientToServer, key, 1, 1));
    auto probe = std::span<std::uint8_t>(datagram).first(kProbeDatagramBytes);
    probe.back() = 1;
    ParsedDirect direct{};
    CHECK(parse_direct(probe, direct) == ParseStatus::InvalidPayload);
}

TEST_CASE("Direct Transport authentication and replay checks retain their compatibility vectors",
          "[patches][direct_transport]") {
    SessionKey key{};
    std::array<std::uint8_t, 64> message{};
    for (std::uint8_t index = 0; index < key.size(); ++index) {
        key[index] = index;
    }
    for (std::uint8_t index = 0; index < message.size(); ++index) {
        message[index] = index;
    }
    constexpr std::array vectors{
        std::pair{std::size_t{0}, 0x726FDB47DD0E0E31ULL},  std::pair{std::size_t{7}, 0xAB0200F58B01D137ULL},
        std::pair{std::size_t{15}, 0xA129CA6149BE45E5ULL}, std::pair{std::size_t{31}, 0x32D892FAD841C342ULL},
        std::pair{std::size_t{63}, 0x958A324CEB064572ULL},
    };
    for (const auto& [length, expected] : vectors) {
        CHECK(sip_hash24(key, std::span(message).first(length)) == expected);
    }

    std::array<std::uint8_t, kDirectHeaderBytes> header{};
    REQUIRE(write_direct_header(header, DirectKind::Data, 0x10203040, 0x50607080, 0));
    const std::array<std::uint8_t, 5> payload{1, 3, 3, 7, 9};
    const auto client_tag = compute_authentication_tag(Direction::ClientToServer, key, header, payload);
    const auto server_tag = compute_authentication_tag(Direction::ServerToClient, key, header, payload);
    CHECK(client_tag != server_tag);
    store_big_endian64(header.data() + offsetof(DirectHeaderV1, authentication_tag), client_tag);
    CHECK(verify_authentication_tag(Direction::ClientToServer, key, header, payload));
    CHECK_FALSE(verify_authentication_tag(Direction::ServerToClient, key, header, payload));

    ReplayWindow replay;
    CHECK(replay.admit(100) == ReplayResult::AcceptedFirst);
    CHECK(replay.admit(102) == ReplayResult::AcceptedNew);
    CHECK(replay.admit(101) == ReplayResult::AcceptedReordered);
    CHECK(replay.admit(101) == ReplayResult::Duplicate);
    CHECK(replay.admit(102U - ReplayWindow::kWidth) == ReplayResult::Stale);
    CHECK(replay.admit(102U + 0x80000000U) == ReplayResult::InvalidJump);

    replay.reset();
    CHECK(replay.admit(0xFFFFFFFEU) == ReplayResult::AcceptedFirst);
    CHECK(replay.admit(0xFFFFFFFFU) == ReplayResult::AcceptedNew);
    CHECK(replay.admit(0) == ReplayResult::AcceptedNew);
    CHECK(replay.admit(0xFFFFFFFEU) == ReplayResult::Duplicate);
}

} // namespace
