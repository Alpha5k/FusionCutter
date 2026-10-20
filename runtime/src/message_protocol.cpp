#include "message_protocol.hpp"

#include <algorithm>
#include <array>
#include <cstring>

namespace fc::runtime::message_protocol {
namespace {

constexpr std::size_t kHeaderBytes = 21;
constexpr std::size_t kDataFieldsBytes = 4;
constexpr std::array kMagic{std::byte{'F'}, std::byte{'C'}, std::byte{'M'}, std::byte{'1'}};

// Checks which handshake tokens must be zero or nonzero for this message kind.
bool valid_tokens(Kind kind, Tokens tokens) noexcept {
    if (tokens.client == 0) {
        return false;
    }
    switch (kind) {
    case Kind::Hello:
        return tokens.host == 0;
    case Kind::Welcome:
    case Kind::Data:
    case Kind::Failed:
    case Kind::Ready:
        return tokens.host != 0;
    }
    return false;
}

// Assembles a little-endian token from eight checked bytes.
std::uint64_t read_u64(std::span<const std::byte> bytes) noexcept {
    std::uint64_t value{};
    for (std::size_t index = 0; index < 8; ++index) {
        value |= std::uint64_t{std::to_integer<std::uint8_t>(bytes[index])} << (index * 8);
    }
    return value;
}

// Reads a little-endian 16-bit integer and advances past its bytes; returns nullopt if truncated.
std::optional<std::uint16_t> read_u16(std::span<const std::byte> &bytes) noexcept {
    if (bytes.size() < 2) {
        return std::nullopt;
    }
    const auto value =
        static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(bytes[0]) | (std::uint16_t{std::to_integer<std::uint8_t>(bytes[1])} << 8));
    bytes = bytes.subspan(2);
    return value;
}

// Returns a view of the next length bytes and advances past them; returns nullopt if truncated.
std::optional<std::string_view> read_id(std::span<const std::byte> &bytes, std::size_t length) noexcept {
    if (length > bytes.size()) {
        return std::nullopt;
    }
    const std::string_view id(reinterpret_cast<const char *>(bytes.data()), length);
    bytes = bytes.subspan(length);
    return id;
}

// Writes a little-endian token into space already checked by the encoder.
void write_u64(std::span<std::byte> &out, std::uint64_t value) noexcept {
    for (std::size_t index = 0; index < 8; ++index) {
        out[index] = static_cast<std::byte>((value >> (index * 8)) & 0xff);
    }
    out = out.subspan(8);
}

// Writes a little-endian 16-bit integer; the encoder must check the value and available space first.
void write_u16(std::span<std::byte> &out, std::size_t value) noexcept {
    out[0] = static_cast<std::byte>(value & 0xff);
    out[1] = static_cast<std::byte>((value >> 8) & 0xff);
    out = out.subspan(2);
}

// Writes the FCM1 signature, message kind and tokens into a destination with at least kHeaderBytes available.
void write_header(std::span<std::byte> &out, Kind kind, Tokens tokens) noexcept {
    std::ranges::copy(kMagic, out.begin());
    out[4] = static_cast<std::byte>(kind);
    out = out.subspan(5);
    write_u64(out, tokens.client);
    write_u64(out, tokens.host);
}

// Copies the ID bytes, including embedded NULs, and advances the output view. The caller checks available space.
void write_id(std::span<std::byte> &out, std::string_view id) noexcept {
    if (!id.empty()) {
        std::memcpy(out.data(), id.data(), id.size());
    }
    out = out.subspan(id.size());
}

} // namespace

bool valid_feature_id(std::string_view id) noexcept {
    if (id.empty()) {
        return false;
    }

    std::size_t index{};
    while (index < id.size()) {
        const auto lead = static_cast<unsigned char>(id[index++]);
        if (lead < 0x80) {
            continue;
        }

        // The leading byte determines the sequence length and the smallest code point it can represent.
        std::uint32_t code_point{}, minimum{};
        std::size_t continuation_count{};
        if (lead >= 0xc2 && lead <= 0xdf) {
            code_point = lead & 0x1f;
            minimum = 0x80;
            continuation_count = 1;
        } else if (lead >= 0xe0 && lead <= 0xef) {
            code_point = lead & 0x0f;
            minimum = 0x800;
            continuation_count = 2;
        } else if (lead >= 0xf0 && lead <= 0xf4) {
            code_point = lead & 0x07;
            minimum = 0x10000;
            continuation_count = 3;
        } else {
            return false;
        }
        if (continuation_count > id.size() - index) {
            return false;
        }

        for (std::size_t continuation = 0; continuation < continuation_count; ++continuation) {
            const auto next = static_cast<unsigned char>(id[index++]);
            if ((next & 0xc0) != 0x80) {
                return false;
            }
            code_point = (code_point << 6) | (next & 0x3f);
        }

        // Reject overlong UTF-8 sequences, surrogate code points and values above Unicode's range.
        if (code_point < minimum || (code_point >= 0xd800 && code_point <= 0xdfff) || code_point > 0x10ffff) {
            return false;
        }
    }
    return true;
}

std::optional<ParsedFrame> parse(std::span<const std::byte> frame) {
    if (frame.size() < kHeaderBytes || frame.size() > kFrameBytes || !std::ranges::equal(frame.first(4), kMagic)) {
        return std::nullopt;
    }

    ParsedFrame parsed;
    parsed.kind = static_cast<Kind>(frame[4]);
    parsed.tokens = {read_u64(frame.subspan(5, 8)), read_u64(frame.subspan(13, 8))};
    if (!valid_tokens(parsed.kind, parsed.tokens)) {
        return std::nullopt;
    }

    auto body = frame.subspan(kHeaderBytes);
    switch (parsed.kind) {
    case Kind::Hello:
    case Kind::Welcome: {
        const auto count = read_u16(body);
        // Each nonempty ID needs two length bytes and at least one text byte. Check the count before reserving storage.
        if (!count || *count > body.size() / 3) {
            return std::nullopt;
        }
        parsed.manifest.reserve(*count);

        // Requiring increasing IDs also rejects duplicates in the advertised feature list.
        for (std::size_t index = 0; index < *count; ++index) {
            const auto length = read_u16(body);
            if (!length) {
                return std::nullopt;
            }
            const auto id = read_id(body, *length);
            if (!id || !valid_feature_id(*id) || (!parsed.manifest.empty() && parsed.manifest.back() >= *id)) {
                return std::nullopt;
            }
            parsed.manifest.push_back(*id);
        }
        break;
    }
    case Kind::Data: {
        const auto id_length = read_u16(body);
        const auto payload_length = read_u16(body);
        if (!id_length || !payload_length) {
            return std::nullopt;
        }
        const auto id = read_id(body, *id_length);
        if (!id || !valid_feature_id(*id) || *payload_length != body.size()) {
            return std::nullopt;
        }
        parsed.feature = *id;
        parsed.payload = body;
        body = body.subspan(body.size());
        break;
    }
    case Kind::Failed: {
        const auto length = read_u16(body);
        if (!length) {
            return std::nullopt;
        }
        const auto id = read_id(body, *length);
        if (!id || (!id->empty() && !valid_feature_id(*id))) {
            return std::nullopt;
        }
        parsed.feature = *id;
        break;
    }
    case Kind::Ready:
        break;
    }

    if (!body.empty()) {
        return std::nullopt;
    }
    return parsed;
}

std::size_t encode_manifest(Kind kind, Tokens tokens, std::span<const std::string_view> manifest, std::span<std::byte> out) noexcept {
    if ((kind != Kind::Hello && kind != Kind::Welcome) || !valid_tokens(kind, tokens)) {
        return 0;
    }

    std::size_t size = kHeaderBytes + 2;
    // Each ID uses at least three bytes; the 2048-byte frame budget also keeps its count and lengths within u16.
    if (manifest.size() > (kFrameBytes - size) / 3) {
        return 0;
    }

    std::string_view previous;
    for (const auto id : manifest) {
        if (kFrameBytes - size < 2 || id.size() > kFrameBytes - size - 2 || !valid_feature_id(id) ||
            (!previous.empty() && previous >= id)) {
            return 0;
        }
        size += 2 + id.size();
        previous = id;
    }
    if (size > out.size()) {
        return 0;
    }

    write_header(out, kind, tokens);
    write_u16(out, manifest.size());
    for (const auto id : manifest) {
        write_u16(out, id.size());
        write_id(out, id);
    }
    return size;
}

std::size_t encode_ready(Tokens tokens, std::span<std::byte> out) noexcept {
    if (!valid_tokens(Kind::Ready, tokens) || out.size() < kHeaderBytes) {
        return 0;
    }

    write_header(out, Kind::Ready, tokens);
    return kHeaderBytes;
}

std::size_t encode_data(Tokens tokens, std::string_view id, std::span<const std::byte> payload, std::span<std::byte> out) noexcept {
    if (!valid_tokens(Kind::Data, tokens) || id.size() > kFrameBytes - kHeaderBytes - kDataFieldsBytes || !valid_feature_id(id)) {
        return 0;
    }
    const auto prefix_size = kHeaderBytes + kDataFieldsBytes + id.size();
    if (payload.size() > kFrameBytes - prefix_size || prefix_size + payload.size() > out.size()) {
        return 0;
    }

    write_header(out, Kind::Data, tokens);
    write_u16(out, id.size());
    write_u16(out, payload.size());
    write_id(out, id);
    std::ranges::copy(payload, out.begin());
    return prefix_size + payload.size();
}

std::size_t encode_failed(Tokens tokens, std::string_view id, std::span<std::byte> out) noexcept {
    if (!valid_tokens(Kind::Failed, tokens) || id.size() > kFrameBytes - kHeaderBytes - 2 || (!id.empty() && !valid_feature_id(id))) {
        return 0;
    }
    const auto size = kHeaderBytes + 2 + id.size();
    if (size > out.size()) {
        return 0;
    }

    write_header(out, Kind::Failed, tokens);
    write_u16(out, id.size());
    write_id(out, id);
    return size;
}

std::size_t payload_capacity(std::string_view id) noexcept {
    if (id.size() > kFrameBytes - kHeaderBytes - kDataFieldsBytes || !valid_feature_id(id)) {
        return 0;
    }
    return kFrameBytes - kHeaderBytes - kDataFieldsBytes - id.size();
}

} // namespace fc::runtime::message_protocol
