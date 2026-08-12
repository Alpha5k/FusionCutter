#include <WinSock2.h>

#include "shared/galaxy.hpp"
#include "shared/diagnostics.hpp"
#include "shared/game_transport.hpp"
#include "shared/native_packet.hpp"
#include "shared/security.hpp"
#include "shared/socket.hpp"
#include "shared/thread_affinity.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <thread>
#include <vector>

namespace {

using namespace fusioncutter::patches::direct_transport;

struct GalaxyFixture {
    std::uint64_t peer{};
    std::uint32_t send_type{};
    std::uint8_t channel{};
    std::array<std::uint8_t, 16> packet{};
    std::uint32_t packet_bytes{};
    std::size_t pops{};

    static bool send(void* context, std::uint64_t peer, const void* bytes, std::uint32_t length,
                     std::uint32_t send_type, std::uint8_t channel) noexcept {
        auto& fixture = *static_cast<GalaxyFixture*>(context);
        fixture.peer = peer;
        fixture.send_type = send_type;
        fixture.channel = channel;
        fixture.packet_bytes = length;
        std::memcpy(fixture.packet.data(), bytes, length);
        return true;
    }

    static bool available(void* context, std::uint32_t* bytes, std::uint8_t channel) noexcept {
        auto& fixture = *static_cast<GalaxyFixture*>(context);
        fixture.channel = channel;
        *bytes = fixture.packet_bytes;
        return true;
    }

    static bool read(void* context, void* bytes, std::uint32_t capacity, std::uint32_t* bytes_read,
                     std::uint64_t* sender, std::uint8_t channel) noexcept {
        auto& fixture = *static_cast<GalaxyFixture*>(context);
        if (capacity < fixture.packet_bytes) {
            return false;
        }
        fixture.channel = channel;
        std::memcpy(bytes, fixture.packet.data(), fixture.packet_bytes);
        *bytes_read = fixture.packet_bytes;
        *sender = fixture.peer;
        return true;
    }

    static void pop(void* context, std::uint8_t channel) noexcept {
        auto& fixture = *static_cast<GalaxyFixture*>(context);
        fixture.channel = channel;
        ++fixture.pops;
    }

    [[nodiscard]] GalaxyApi api() noexcept {
        return {this, &send, &available, &read, &pop};
    }
};

std::array<std::uint8_t, 2048> g_packet{};
bool g_small_allocation{};

void* __fastcall allocate_packet(bool small) {
    g_small_allocation = small;
    return g_packet.data();
}

void __fastcall initialize_packet(void* packet) {
    std::memset(packet, 0, g_packet.size());
}

struct RandomFixture {
    std::vector<std::uint32_t> values;
    std::size_t next{};

    static bool fill(void* context, std::uint8_t* output, std::size_t bytes) noexcept {
        auto& fixture = *static_cast<RandomFixture*>(context);
        if (bytes != sizeof(std::uint32_t) || fixture.next >= fixture.values.size()) {
            return false;
        }
        store_big_endian32(output, fixture.values[fixture.next++]);
        return true;
    }

    [[nodiscard]] RandomSource source() noexcept {
        return {this, &fill};
    }
};

[[nodiscard]] std::uint32_t ipv4(std::uint8_t first, std::uint8_t second, std::uint8_t third,
                                 std::uint8_t fourth) noexcept {
    return htonl((static_cast<std::uint32_t>(first) << 24) | (static_cast<std::uint32_t>(second) << 16) |
                 (static_cast<std::uint32_t>(third) << 8) | fourth);
}

TEST_CASE("Direct Transport Galaxy adapter preserves bounded control carrier behavior", "[patches][direct_transport]") {
    const std::array<std::uint8_t, 4> bytes{1, 2, 3, 4};
    GalaxyFixture fixture;
    GalaxyNetworking galaxy(fixture.api());
    REQUIRE(galaxy.valid());
    REQUIRE(galaxy.send_reliable_immediate(0x1122334455667788ULL, bytes, kControlChannel));
    CHECK(fixture.peer == 0x1122334455667788ULL);
    CHECK(fixture.send_type == kGalaxyReliableImmediate);
    std::uint32_t available{};
    REQUIRE(galaxy.packet_available(available, kControlChannel));
    CHECK(available == bytes.size());
    std::array<std::uint8_t, 16> received{};
    std::uint32_t bytes_read{};
    std::uint64_t sender{};
    REQUIRE(galaxy.read_packet(received, bytes_read, sender, kControlChannel));
    CHECK(std::ranges::equal(bytes, std::span(received).first(bytes_read)));
    CHECK(galaxy.pop_packet(kControlChannel));
    CHECK(fixture.pops == 1);
}

TEST_CASE("Direct Transport native packet and identity adapters preserve the game layout",
          "[patches][direct_transport]") {
    std::array<std::byte, kNativeEndpointTableRequiredBytes> endpoint_storage{};
    auto* endpoint_table = endpoint_storage.data() + kNativeEndpointConnectedOffset;
    constexpr std::uint8_t slot = 17;
    auto* record = endpoint_table + slot * kNativeEndpointStride;
    constexpr std::uint64_t expected_identity = 0x1122334455667788ULL;
    std::memcpy(record, &expected_identity, sizeof(expected_identity));
    record[-static_cast<std::ptrdiff_t>(kNativeEndpointConnectedOffset)] = std::byte{1};
    std::uint64_t identity{};
    REQUIRE(read_native_identity(endpoint_table, slot, identity));
    CHECK(identity == expected_identity);
    record[-static_cast<std::ptrdiff_t>(kNativeEndpointConnectedOffset)] = std::byte{};
    CHECK_FALSE(read_native_identity(endpoint_table, slot, identity));

    std::uint32_t prefix = 3;
    const NativePacketFactory factory{
        &allocate_packet,
        reinterpret_cast<NativePacketInitialize>(&initialize_packet),
        &prefix,
    };
    const std::array<std::uint8_t, 5> payload{0x0B, 1, 2, 3, 4};
    void* packet{};
    REQUIRE(build_native_packet(factory, payload, packet) == NativePacketResult::Built);
    CHECK(packet == g_packet.data());
    CHECK(g_small_allocation);
    CHECK(g_packet[8] == 3);
    CHECK(std::memcmp(g_packet.data() + 0x0F, payload.data(), payload.size()) == 0);

    prefix = 4;
    CHECK(build_native_packet(factory, payload, packet) == NativePacketResult::InvalidPrefix);
    CHECK(packet == nullptr);

    const NativePacketFactory unavailable{};
    CHECK(build_native_packet(unavailable, payload, packet) == NativePacketResult::FactoryUnavailable);
}

TEST_CASE("Direct Transport retains one carrier and generation across nested native groups",
          "[patches][direct_transport]") {
    TransmitGroupState group;
    CHECK_FALSE(group.active());

    REQUIRE(group.begin(7, Carrier::Direct));
    CHECK(group.active());
    CHECK(group.belongs_to(7));
    CHECK(group.carrier() == Carrier::Direct);

    REQUIRE(group.begin(8, Carrier::Galaxy));
    CHECK(group.belongs_to(7));
    CHECK_FALSE(group.belongs_to(8));
    CHECK(group.carrier() == Carrier::Direct);

    REQUIRE(group.end());
    CHECK(group.active());
    REQUIRE(group.end());
    CHECK_FALSE(group.active());
    CHECK(group.carrier() == Carrier::Galaxy);
    CHECK_FALSE(group.end());
}

TEST_CASE("Direct Transport admits a bounded receive batch and drains the remaining datagrams",
          "[patches][direct_transport]") {
    std::uint32_t next{};
    std::uint32_t handled{};
    const auto drained = drain_datagrams(
        [&](Endpoint&) noexcept {
            return next++ < kReceiveAdmissionLimit + 3 ? SocketResult{5, 0} : SocketResult{-1, kSocketWouldBlock};
        },
        [&](std::int32_t bytes, const Endpoint&) noexcept {
            CHECK(bytes == 5);
            ++handled;
        });

    CHECK(handled == kReceiveAdmissionLimit);
    CHECK(drained.discarded == 3);
    CHECK(drained.error == 0);
    CHECK(next == kReceiveAdmissionLimit + 4);

    next = 0;
    handled = 0;
    const auto oversized = drain_datagrams(
        [&](Endpoint&) noexcept {
            ++next;
            if (next == 1) {
                return SocketResult{-1, kSocketMessageTooLarge};
            }
            return next == 2 ? SocketResult{5, 0} : SocketResult{-1, kSocketWouldBlock};
        },
        [&](std::int32_t, const Endpoint&) noexcept {
            ++handled;
        });
    CHECK(handled == 1);
    CHECK(oversized.discarded == 0);
    CHECK(oversized.error == 0);
}

TEST_CASE("Direct Transport rejects callbacks from outside its claimed network thread", "[patches][direct_transport]") {
    NetworkThreadAffinity affinity;
    REQUIRE(affinity.claim_current());
    CHECK(affinity.is_current());

    bool accepted{};
    std::thread other([&] {
        accepted = affinity.is_current();
    });
    other.join();

    CHECK_FALSE(accepted);
    CHECK(affinity.rejected_calls() == 1);
}

TEST_CASE("Direct Transport endpoint and connection admission rejects ambiguous inputs",
          "[patches][direct_transport]") {
    constexpr Endpoint unbound{};
    constexpr Endpoint first{0x0100007F, 3658};
    constexpr Endpoint second{0x0200007F, 3658};
    static_assert(endpoint_admits_source(unbound, first, true));
    static_assert(!endpoint_admits_source(unbound, first, false));
    static_assert(endpoint_admits_source(first, first, false));
    static_assert(!endpoint_admits_source(first, second, true));

    CHECK_FALSE(is_public_ipv4(ipv4(10, 0, 0, 1)));
    CHECK_FALSE(is_public_ipv4(ipv4(172, 31, 255, 255)));
    CHECK_FALSE(is_public_ipv4(ipv4(192, 168, 1, 1)));
    CHECK_FALSE(is_public_ipv4(ipv4(203, 0, 113, 1)));
    CHECK(is_public_ipv4(ipv4(1, 1, 1, 1)));
    CHECK(is_public_ipv4(ipv4(172, 32, 0, 1)));

    RandomFixture random{{0, 7, 8}};
    const std::array<std::uint32_t, 1> live{7};
    std::uint32_t connection_id{};
    REQUIRE(generate_connection_id(random.source(), live, connection_id));
    CHECK(connection_id == 8);
    CHECK(random.next == 3);
}

} // namespace
