#include "protocol.hpp"

#include <algorithm>
#include <array>
#include <cstring>

namespace fusioncutter::patches::direct_transport {
namespace {

constexpr std::array<std::uint8_t, 4> kControlMagic{'B', 'F', '2', 'C'};
constexpr std::array<std::uint8_t, 4> kDirectMagic{'B', 'F', '2', 'D'};

void write_control_header(ControlHeaderV1& header, ControlKind kind, std::size_t bytes) noexcept {
    header.magic = kControlMagic;
    header.version = kProtocolVersion;
    header.kind = static_cast<std::uint8_t>(kind);
    store_big_endian16(header.total_bytes.data(), static_cast<std::uint16_t>(bytes));
}

[[nodiscard]] bool is_zero_payload(std::span<const std::uint8_t> bytes) noexcept {
    std::uint8_t combined{};
    for (const auto value : bytes) {
        combined = static_cast<std::uint8_t>(combined | value);
    }
    return combined == 0;
}

template <typename Message> [[nodiscard]] Message read_message(std::span<const std::uint8_t> bytes) noexcept {
    Message message{};
    std::memcpy(&message, bytes.data(), sizeof(message));
    return message;
}

template <typename Message> void write_message(std::span<std::uint8_t> output, const Message& message) noexcept {
    std::memcpy(output.data(), &message, sizeof(message));
}

} // namespace

std::uint16_t load_big_endian16(const void* bytes) noexcept {
    const auto* value = static_cast<const std::uint8_t*>(bytes);
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(value[0]) << 8) |
                                      static_cast<std::uint16_t>(value[1]));
}

std::uint32_t load_big_endian32(const void* bytes) noexcept {
    const auto* value = static_cast<const std::uint8_t*>(bytes);
    return (static_cast<std::uint32_t>(value[0]) << 24) | (static_cast<std::uint32_t>(value[1]) << 16) |
           (static_cast<std::uint32_t>(value[2]) << 8) | static_cast<std::uint32_t>(value[3]);
}

std::uint64_t load_big_endian64(const void* bytes) noexcept {
    const auto* value = static_cast<const std::uint8_t*>(bytes);
    std::uint64_t result{};
    for (std::size_t index = 0; index < 8; ++index) {
        result = (result << 8) | value[index];
    }
    return result;
}

void store_big_endian16(void* bytes, std::uint16_t value) noexcept {
    auto* output = static_cast<std::uint8_t*>(bytes);
    output[0] = static_cast<std::uint8_t>(value >> 8);
    output[1] = static_cast<std::uint8_t>(value);
}

void store_big_endian32(void* bytes, std::uint32_t value) noexcept {
    auto* output = static_cast<std::uint8_t*>(bytes);
    output[0] = static_cast<std::uint8_t>(value >> 24);
    output[1] = static_cast<std::uint8_t>(value >> 16);
    output[2] = static_cast<std::uint8_t>(value >> 8);
    output[3] = static_cast<std::uint8_t>(value);
}

void store_big_endian64(void* bytes, std::uint64_t value) noexcept {
    auto* output = static_cast<std::uint8_t*>(bytes);
    for (std::size_t index = 0; index < 8; ++index) {
        output[7 - index] = static_cast<std::uint8_t>(value >> (index * 8));
    }
}

std::size_t control_bytes(ControlKind kind) noexcept {
    switch (kind) {
    case ControlKind::Caps:
        return kCapsBytes;
    case ControlKind::Offer:
        return kOfferBytes;
    case ControlKind::Ready:
    case ControlKind::Commit:
        return kActivationBytes;
    default:
        return 0;
    }
}

ParseStatus parse_control(std::span<const std::uint8_t> bytes, ParsedControl& output) noexcept {
    output = {};
    if (bytes.size() < kControlHeaderBytes) {
        return ParseStatus::TooShort;
    }

    const auto header = read_message<ControlHeaderV1>(bytes);
    if (header.magic != kControlMagic) {
        return ParseStatus::BadMagic;
    }
    if (load_big_endian16(header.total_bytes.data()) != bytes.size()) {
        return ParseStatus::BadLength;
    }
    output.version = header.version;
    if (header.kind < static_cast<std::uint8_t>(ControlKind::Caps) ||
        header.kind > static_cast<std::uint8_t>(ControlKind::Commit)) {
        return ParseStatus::UnknownKind;
    }

    output.kind = static_cast<ControlKind>(header.kind);
    if (bytes.size() != control_bytes(output.kind)) {
        return ParseStatus::BadLength;
    }
    if (header.version != kProtocolVersion) {
        return ParseStatus::UnsupportedVersion;
    }

    switch (output.kind) {
    case ControlKind::Caps: {
        const auto message = read_message<CapsV1>(bytes);
        output.client_nonce = load_big_endian64(message.client_nonce.data());
        break;
    }
    case ControlKind::Offer: {
        const auto message = read_message<OfferV1>(bytes);
        output.client_nonce = load_big_endian64(message.client_nonce.data());
        output.connection_id = load_big_endian32(message.connection_id.data());
        std::memcpy(&output.server_ipv4_network_order, message.server_ipv4.data(), message.server_ipv4.size());
        output.server_port = load_big_endian16(message.server_port.data());
        output.session_key = message.session_key;
        break;
    }
    case ControlKind::Ready:
    case ControlKind::Commit: {
        const auto message = read_message<ActivationV1>(bytes);
        output.connection_id = load_big_endian32(message.connection_id.data());
        break;
    }
    default:
        return ParseStatus::UnknownKind;
    }
    return ParseStatus::Ok;
}

bool write_caps(std::span<std::uint8_t> output, std::uint64_t client_nonce) noexcept {
    if (output.size() != kCapsBytes) {
        return false;
    }
    CapsV1 message{};
    write_control_header(message.header, ControlKind::Caps, sizeof(message));
    store_big_endian64(message.client_nonce.data(), client_nonce);
    write_message(output, message);
    return true;
}

bool write_offer(std::span<std::uint8_t> output, std::uint64_t client_nonce, std::uint32_t connection_id,
                 std::uint32_t server_ipv4_network_order, std::uint16_t server_port,
                 std::span<const std::uint8_t, 16> session_key) noexcept {
    if (output.size() != kOfferBytes) {
        return false;
    }
    OfferV1 message{};
    write_control_header(message.header, ControlKind::Offer, sizeof(message));
    store_big_endian64(message.client_nonce.data(), client_nonce);
    store_big_endian32(message.connection_id.data(), connection_id);
    std::memcpy(message.server_ipv4.data(), &server_ipv4_network_order, message.server_ipv4.size());
    store_big_endian16(message.server_port.data(), server_port);
    std::ranges::copy(session_key, message.session_key.begin());
    write_message(output, message);
    return true;
}

bool write_activation(std::span<std::uint8_t> output, ControlKind kind, std::uint32_t connection_id) noexcept {
    if (output.size() != kActivationBytes || (kind != ControlKind::Ready && kind != ControlKind::Commit)) {
        return false;
    }
    ActivationV1 message{};
    write_control_header(message.header, kind, sizeof(message));
    store_big_endian32(message.connection_id.data(), connection_id);
    write_message(output, message);
    return true;
}

ParseStatus parse_direct(std::span<const std::uint8_t> bytes, ParsedDirect& output) noexcept {
    output = {};
    if (bytes.size() < kDirectHeaderBytes) {
        return ParseStatus::TooShort;
    }

    const auto header = read_message<DirectHeaderV1>(bytes);
    if (header.magic != kDirectMagic) {
        return ParseStatus::BadMagic;
    }
    if (header.version != kProtocolVersion) {
        return ParseStatus::UnsupportedVersion;
    }
    if (load_big_endian16(header.header_bytes.data()) != kDirectHeaderBytes) {
        return ParseStatus::BadLength;
    }
    if (header.kind < static_cast<std::uint8_t>(DirectKind::Data) ||
        header.kind > static_cast<std::uint8_t>(DirectKind::ProbeAck)) {
        return ParseStatus::UnknownKind;
    }

    output.kind = static_cast<DirectKind>(header.kind);
    output.connection_id = load_big_endian32(header.connection_id.data());
    output.datagram_sequence = load_big_endian32(header.datagram_sequence.data());
    output.authentication_tag = load_big_endian64(header.authentication_tag.data());
    output.payload = bytes.subspan(kDirectHeaderBytes);

    if (output.kind == DirectKind::Data) {
        if (output.payload.size() < kNativeHeaderBytes || output.payload.size() > kMaximumNativeBytes) {
            return ParseStatus::BadLength;
        }
    } else if (bytes.size() != kProbeDatagramBytes) {
        return ParseStatus::BadLength;
    } else if (!is_zero_payload(output.payload)) {
        return ParseStatus::InvalidPayload;
    }
    return ParseStatus::Ok;
}

bool write_direct_header(std::span<std::uint8_t> output, DirectKind kind, std::uint32_t connection_id,
                         std::uint32_t datagram_sequence, std::uint64_t authentication_tag) noexcept {
    if (output.size() < kDirectHeaderBytes || kind < DirectKind::Data || kind > DirectKind::ProbeAck) {
        return false;
    }
    DirectHeaderV1 header{};
    header.magic = kDirectMagic;
    header.version = kProtocolVersion;
    header.kind = static_cast<std::uint8_t>(kind);
    store_big_endian16(header.header_bytes.data(), static_cast<std::uint16_t>(kDirectHeaderBytes));
    store_big_endian32(header.connection_id.data(), connection_id);
    store_big_endian32(header.datagram_sequence.data(), datagram_sequence);
    store_big_endian64(header.authentication_tag.data(), authentication_tag);
    write_message(output, header);
    return true;
}

} // namespace fusioncutter::patches::direct_transport
