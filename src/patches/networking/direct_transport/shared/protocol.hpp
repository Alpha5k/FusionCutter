#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace fusioncutter::patches::direct_transport {

inline constexpr std::uint8_t kProtocolVersion = 1;
inline constexpr std::uint8_t kControlChannel = 63;
inline constexpr std::uint64_t kHandshakeRetryIntervalMs = 1'000;
inline constexpr std::uint64_t kHandshakeDeadlineMs = 5'000;
inline constexpr std::uint8_t kHandshakeAttemptLimit = 3;
inline constexpr std::uint64_t kControlPumpIntervalMs = 10;
inline constexpr std::uint8_t kControlDrainLimit = 16;
inline constexpr std::uint8_t kControlAssociationLimit = 16;
inline constexpr std::size_t kControlHeaderBytes = 8;
inline constexpr std::size_t kCapsBytes = 16;
inline constexpr std::size_t kOfferBytes = 42;
inline constexpr std::size_t kActivationBytes = 12;
inline constexpr std::size_t kDirectHeaderBytes = 24;
inline constexpr std::size_t kNativeHeaderBytes = 5;
inline constexpr std::size_t kMaximumNativeBytes = 1'012;
inline constexpr std::size_t kProbePayloadBytes = 1'012;
inline constexpr std::size_t kProbeDatagramBytes = kDirectHeaderBytes + kProbePayloadBytes;
inline constexpr std::size_t kMaximumDirectDatagramBytes = kDirectHeaderBytes + kMaximumNativeBytes;

enum class Policy : std::uint8_t {
    Disabled = 1,
    PreferDirect = 2,
    RequireDirectPatched = 3,
    RequireDirectAll = 4,
};

enum class RouteState : std::uint8_t {
    Unclassified = 1,
    Negotiating = 2,
    AwaitCommit = 3,
    GalaxyLocked = 4,
    DirectLocked = 5,
};

enum class Carrier : std::uint8_t {
    Galaxy = 1,
    Direct = 2,
};

enum class Direction : std::uint8_t {
    ClientToServer = 1,
    ServerToClient = 2,
};

enum class TransmitRoute : std::uint8_t {
    Galaxy = 1,
    DirectPending = 2,
    Direct = 3,
};

enum class ControlKind : std::uint8_t {
    Caps = 1,
    Offer = 2,
    Ready = 3,
    Commit = 4,
};

enum class DirectKind : std::uint8_t {
    Data = 1,
    Probe = 2,
    ProbeAck = 3,
};

// Tells the final-send hook whether Direct handled a native packet and which result to return.
struct NativeTransmitResult {
    enum class Failure : std::uint8_t {
        None,
        Serialization,
        Socket,
        PartialSend,
    };

    std::int32_t result{};
    std::int32_t error{};
    bool handled{};
    Failure failure{};
};

enum class ParseStatus : std::uint8_t {
    Ok,
    TooShort,
    BadMagic,
    BadLength,
    UnsupportedVersion,
    UnknownKind,
    InvalidPayload,
};

// Fixed on-wire messages for the Galaxy control channel and authenticated UDP transport.
#pragma pack(push, 1)
struct ControlHeaderV1 {
    std::array<std::uint8_t, 4> magic;
    std::uint8_t version;
    std::uint8_t kind;
    std::array<std::uint8_t, 2> total_bytes;
};

struct CapsV1 {
    ControlHeaderV1 header;
    std::array<std::uint8_t, 8> client_nonce;
};

struct OfferV1 {
    ControlHeaderV1 header;
    std::array<std::uint8_t, 8> client_nonce;
    std::array<std::uint8_t, 4> connection_id;
    std::array<std::uint8_t, 4> server_ipv4;
    std::array<std::uint8_t, 2> server_port;
    std::array<std::uint8_t, 16> session_key;
};

struct ActivationV1 {
    ControlHeaderV1 header;
    std::array<std::uint8_t, 4> connection_id;
};

struct DirectHeaderV1 {
    std::array<std::uint8_t, 4> magic;
    std::uint8_t version;
    std::uint8_t kind;
    std::array<std::uint8_t, 2> header_bytes;
    std::array<std::uint8_t, 4> connection_id;
    std::array<std::uint8_t, 4> datagram_sequence;
    std::array<std::uint8_t, 8> authentication_tag;
};
#pragma pack(pop)

static_assert(sizeof(ControlHeaderV1) == kControlHeaderBytes);
static_assert(sizeof(CapsV1) == kCapsBytes);
static_assert(sizeof(OfferV1) == kOfferBytes);
static_assert(sizeof(ActivationV1) == kActivationBytes);
static_assert(sizeof(DirectHeaderV1) == kDirectHeaderBytes);

struct ParsedControl {
    ControlKind kind{};
    std::uint8_t version{};
    std::uint64_t client_nonce{};
    std::uint32_t connection_id{};
    std::uint32_t server_ipv4_network_order{};
    std::uint16_t server_port{};
    std::array<std::uint8_t, 16> session_key{};
};

struct ParsedDirect {
    DirectKind kind{};
    std::uint32_t connection_id{};
    std::uint32_t datagram_sequence{};
    std::uint64_t authentication_tag{};
    std::span<const std::uint8_t> payload{};
};

// Converts the protocol's fixed-width integers to and from network byte order.
[[nodiscard]] std::uint16_t load_big_endian16(const void* bytes) noexcept;
[[nodiscard]] std::uint32_t load_big_endian32(const void* bytes) noexcept;
[[nodiscard]] std::uint64_t load_big_endian64(const void* bytes) noexcept;
void store_big_endian16(void* bytes, std::uint16_t value) noexcept;
void store_big_endian32(void* bytes, std::uint32_t value) noexcept;
void store_big_endian64(void* bytes, std::uint64_t value) noexcept;

// Parses and serializes the Galaxy control-channel handshake messages.
[[nodiscard]] std::size_t control_bytes(ControlKind kind) noexcept;
[[nodiscard]] ParseStatus parse_control(std::span<const std::uint8_t> bytes, ParsedControl& output) noexcept;
[[nodiscard]] bool write_caps(std::span<std::uint8_t> output, std::uint64_t client_nonce) noexcept;
[[nodiscard]] bool write_offer(std::span<std::uint8_t> output, std::uint64_t client_nonce, std::uint32_t connection_id,
                               std::uint32_t server_ipv4_network_order, std::uint16_t server_port,
                               std::span<const std::uint8_t, 16> session_key) noexcept;
[[nodiscard]] bool write_activation(std::span<std::uint8_t> output, ControlKind kind,
                                    std::uint32_t connection_id) noexcept;

// Parses and serializes the authenticated UDP frame shared by data and probe messages.
[[nodiscard]] ParseStatus parse_direct(std::span<const std::uint8_t> bytes, ParsedDirect& output) noexcept;
[[nodiscard]] bool write_direct_header(std::span<std::uint8_t> output, DirectKind kind, std::uint32_t connection_id,
                                       std::uint32_t datagram_sequence, std::uint64_t authentication_tag) noexcept;

} // namespace fusioncutter::patches::direct_transport
