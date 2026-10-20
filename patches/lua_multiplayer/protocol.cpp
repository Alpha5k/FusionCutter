#include "protocol.hpp"

#include <Windows.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <optional>
#include <string>

namespace fc::core_plugin::lua_multiplayer::protocol {
namespace {

constexpr std::uint8_t kListeners = 1;
constexpr std::uint8_t kMessage = 2;
constexpr unsigned kTableDepth = 16;

// Appends fields within the caller's byte limit and retains the first encoding error.
struct Writer {
    std::vector<std::byte> bytes;
    std::size_t limit;
    std::optional<EncodeError> error;

    // Records the first error and returns false to stop encoding.
    bool refuse(EncodeError reason) {
        if (!error) {
            error = reason;
        }
        return false;
    }

    // Writes count low bytes in little-endian order if the whole field fits the byte limit.
    bool integer(std::uint64_t value, std::size_t count) {
        if (count > limit - bytes.size()) {
            return refuse(EncodeError::TooLarge);
        }

        for (std::size_t i = 0; i < count; ++i) {
            bytes.push_back(static_cast<std::byte>((value >> (i * 8)) & 0xff));
        }
        return true;
    }

    // Copies a complete byte field, including embedded NULs, if it fits the byte limit.
    bool copy(fc::Bytes value) {
        if (value.size() > limit - bytes.size()) {
            return refuse(EncodeError::TooLarge);
        }
        bytes.insert(bytes.end(), value.begin(), value.end());
        return true;
    }
};

// Reads packet fields and tracks the first unread byte.
struct Reader {
    fc::Bytes bytes;
    std::size_t position{};

    fc::Bytes remaining() const noexcept {
        return bytes.subspan(position);
    }

    // Reads a complete little-endian integer of type T and advances past its bytes.
    template <class T> std::optional<T> integer() {
        if (sizeof(T) > remaining().size()) {
            return std::nullopt;
        }

        std::uint64_t value{};
        for (std::size_t i = 0; i < sizeof(T); ++i) {
            value |= std::to_integer<std::uint64_t>(bytes[position++]) << (i * 8);
        }
        return static_cast<T>(value);
    }

    // Borrows a complete byte field and advances only when every requested byte exists.
    std::optional<fc::Bytes> take(std::size_t count) {
        if (count > remaining().size()) {
            return std::nullopt;
        }
        const auto value = remaining().first(count);
        position += count;
        return value;
    }
};

// Views string storage as bytes, retaining embedded NULs.
fc::Bytes bytes(std::string_view text) noexcept {
    return {reinterpret_cast<const std::byte *>(text.data()), text.size()};
}

// Checks that a finite number survives conversion to the receiving game's float32 Lua type exactly.
bool representable(double number) noexcept {
    constexpr auto maximum = static_cast<double>(std::numeric_limits<float>::max());
    if (!std::isfinite(number) || number > maximum || number < -maximum) {
        return false;
    }

    const auto narrowed = static_cast<float>(number);
    return static_cast<double>(narrowed) == number;
}

// Encodes borrowed Lua values and fills in each table's entry count after traversal.
struct Encoder {
    Writer output;

    // Appends a supported value; key permits only booleans, numbers and strings, and depth counts enclosing tables.
    bool value(fc::lua::Value input, unsigned depth, bool key = false) {
        const auto type = input.type();
        if (key && type != fc::lua::Type::Boolean && type != fc::lua::Type::Number && type != fc::lua::Type::String) {
            return output.refuse(EncodeError::InvalidValue);
        }

        switch (type) {
        case fc::lua::Type::Nil:
            return output.integer(0, 1);
        case fc::lua::Type::Boolean: {
            const auto boolean = input.as_bool();
            return boolean ? output.integer(*boolean ? 2u : 1u, 1) : output.refuse(EncodeError::InvalidValue);
        }
        case fc::lua::Type::Number: {
            const auto number = input.as_number();
            if (!number || !representable(*number)) {
                return output.refuse(EncodeError::InvalidValue);
            }

            // Wire numbers use binary64 even though Lua accepts only values exactly representable in float32.
            return output.integer(3, 1) && output.integer(std::bit_cast<std::uint64_t>(*number), 8);
        }
        case fc::lua::Type::String: {
            const auto text = input.as_string();
            if (!text) {
                return output.refuse(EncodeError::InvalidValue);
            }
            if (text->size() > UINT16_MAX) {
                return output.refuse(EncodeError::TooLarge);
            }
            return output.integer(4, 1) && output.integer(text->size(), 2) && output.copy(bytes(*text));
        }
        case fc::lua::Type::Table: {
            if (depth == kTableDepth || input.has_metatable()) {
                return output.refuse(EncodeError::InvalidValue);
            }
            if (!output.integer(5, 1)) {
                return false;
            }

            // Reserve the two-byte entry count before traversal reveals how many pairs fit.
            const auto count_position = output.bytes.size();
            if (!output.integer(0, 2)) {
                return false;
            }
            std::uint16_t count{};
            // Stop at the first error; depth and byte limits bound cycles, and shared tables are copied independently.
            const auto traversal = input.for_each([&](fc::lua::Value table_key, fc::lua::Value table_value) {
                if (count == UINT16_MAX) {
                    return output.refuse(EncodeError::TooLarge);
                }
                if (!value(table_key, depth + 1, true) || !value(table_value, depth + 1)) {
                    return false;
                }
                ++count;
                return true;
            });
            if (output.error) {
                return false;
            }
            if (!traversal || !*traversal) {
                return output.refuse(EncodeError::InvalidValue);
            }

            // Fill the reserved count in little-endian order after every pair has been encoded.
            output.bytes[count_position] = static_cast<std::byte>(count & 0xff);
            output.bytes[count_position + 1] = static_cast<std::byte>(count >> 8);
            return true;
        }
        default:
            return output.refuse(EncodeError::InvalidValue);
        }
    }
};

// Builds values in the supplied frame and tracks table keys independently of their decoded values.
struct Decoder {
    fc::lua::Frame &frame;
    Reader input;

    // Decodes a supported value; depth counts enclosing tables, and key permits only booleans, numbers and strings.
    fc::Result<fc::lua::Value> value(unsigned depth, bool key = false) {
        const auto tag = input.integer<std::uint8_t>();
        if (!tag) {
            return std::unexpected(fc::Error{"Lua message value is truncated"});
        }
        if (key && *tag != 1 && *tag != 2 && *tag != 3 && *tag != 4) {
            return std::unexpected(fc::Error{"Lua table key type is unsupported"});
        }

        switch (*tag) {
        case 0:
            return frame.nil();
        case 1:
            return frame.value(false);
        case 2:
            return frame.value(true);
        case 3: {
            const auto bits = input.integer<std::uint64_t>();
            if (!bits) {
                return std::unexpected(fc::Error{"Lua number is truncated"});
            }
            const auto number = std::bit_cast<double>(*bits);
            if (!representable(number)) {
                return std::unexpected(fc::Error{"Lua number must be finite and exactly representable as float32"});
            }
            return frame.value(number);
        }
        case 4: {
            const auto count = input.integer<std::uint16_t>();
            if (!count) {
                return std::unexpected(fc::Error{"Lua string length is truncated"});
            }
            const auto text = input.take(*count);
            if (!text) {
                return std::unexpected(fc::Error{"Lua string bytes are truncated"});
            }
            return frame.value(std::string_view{reinterpret_cast<const char *>(text->data()), text->size()});
        }
        case 5: {
            if (depth == kTableDepth) {
                return std::unexpected(fc::Error{"Lua table nesting exceeds 16 levels"});
            }

            // Each pair requires at least two tag bytes before any key or value fields.
            const auto count = input.integer<std::uint16_t>();
            if (!count || *count > input.remaining().size() / 2) {
                return std::unexpected(fc::Error{"Lua table pairs are truncated"});
            }

            auto table = frame.table();
            auto seen = frame.table();
            for (std::uint16_t i = 0; i < *count; ++i) {
                auto table_key = value(depth + 1, true);
                if (!table_key) {
                    return std::unexpected(std::move(table_key.error()));
                }
                if (seen.get(*table_key).type() != fc::lua::Type::Nil) {
                    return std::unexpected(fc::Error{"Lua table contains a duplicate key"});
                }

                // Track every key separately because setting a nil value removes the field from the result table.
                seen.set(*table_key, frame.value(true));
                auto table_value = value(depth + 1);
                if (!table_value) {
                    return std::unexpected(std::move(table_value.error()));
                }
                table.set(*table_key, *table_value);
            }
            return table;
        }
        default:
            return std::unexpected(fc::Error{"Lua message value tag is unsupported"});
        }
    }
};

} // namespace

bool valid_channel(std::string_view channel) noexcept {
    if (channel.empty() || channel.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return false;
    }
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, channel.data(), static_cast<int>(channel.size()), nullptr, 0) > 0;
}

fc::Result<std::vector<std::byte>> encode_listeners(std::span<const Listener> listeners, std::size_t max_bytes) {
    if (listeners.size() > UINT16_MAX) {
        return std::unexpected(fc::Error{"Lua listener count exceeds the 16-bit packet count limit"});
    }
    Writer output{{}, max_bytes, {}};
    if (!output.integer(kListeners, 1) || !output.integer(listeners.size(), 2)) {
        return std::unexpected(fc::Error{"Complete Lua listener list exceeds the feature payload limit"});
    }

    for (const auto &entry : listeners) {
        if (entry.channel.size() > UINT16_MAX || !output.integer(entry.registration, 8) || !output.integer(entry.channel.size(), 2) ||
            !output.copy(bytes(entry.channel))) {
            return std::unexpected(
                fc::Error{"Lua channel length exceeds 65535 bytes or the complete listener list exceeds the feature payload limit"});
        }
    }
    return std::move(output.bytes);
}

fc::Result<std::map<std::string, std::uint64_t, std::less<>>> decode_listeners(fc::Bytes packet) {
    Reader input{packet};
    const auto kind = input.integer<std::uint8_t>();
    const auto count = input.integer<std::uint16_t>();
    // Each listener needs eight registration bytes, two length bytes and at least one channel byte.
    if (!kind || *kind != kListeners || !count || *count > input.remaining().size() / 11) {
        return std::unexpected(fc::Error{"Lua listener packet has an invalid kind or truncated entries"});
    }

    std::map<std::string, std::uint64_t, std::less<>> listeners;
    for (std::uint16_t i = 0; i < *count; ++i) {
        const auto registration = input.integer<std::uint64_t>();
        const auto size = input.integer<std::uint16_t>();
        if (!registration || !*registration || !size || !*size) {
            return std::unexpected(fc::Error{"Lua listener registration or channel length is invalid"});
        }
        const auto text = input.take(*size);
        if (!text) {
            return std::unexpected(fc::Error{"Lua listener channel is truncated"});
        }

        const auto channel = std::string_view{reinterpret_cast<const char *>(text->data()), text->size()};
        if (!valid_channel(channel)) {
            return std::unexpected(fc::Error{"Lua listener channel is not valid UTF-8"});
        }
        if (std::ranges::any_of(listeners,
                                [&](const auto &prior) {
                                    return prior.second == *registration;
                                }) ||
            !listeners.emplace(channel, *registration).second) {
            return std::unexpected(fc::Error{"Lua listener list repeats a registration or channel"});
        }
    }

    if (!input.remaining().empty()) {
        return std::unexpected(fc::Error{"Lua listener packet has trailing bytes"});
    }
    return listeners;
}

fc::Result<std::vector<std::byte>> encode_message(std::uint64_t destination, fc::Bytes value, std::size_t max_bytes) {
    if (max_bytes < kMessageHeaderBytes || value.size() > max_bytes - kMessageHeaderBytes) {
        return std::unexpected(fc::Error{"Lua message exceeds the feature payload limit"});
    }

    Writer output{{}, max_bytes, {}};
    output.integer(kMessage, 1);
    output.integer(destination, 8);
    output.copy(value);
    return std::move(output.bytes);
}

fc::Result<Message> decode_message(fc::Bytes packet) {
    Reader input{packet};
    const auto kind = input.integer<std::uint8_t>();
    const auto destination = input.integer<std::uint64_t>();
    if (!kind || *kind != kMessage || !destination || !*destination || input.remaining().empty()) {
        return std::unexpected(fc::Error{"Lua message packet has an invalid kind, destination or missing value"});
    }
    return Message{*destination, input.remaining()};
}

std::expected<std::vector<std::byte>, EncodeError> encode_value(fc::lua::Value value, std::size_t max_bytes) {
    Encoder encoder{{{}, max_bytes, {}}};
    if (!encoder.value(value, 0)) {
        return std::unexpected(*encoder.output.error);
    }
    if (!value.result()) {
        return std::unexpected(EncodeError::InvalidValue);
    }
    return std::move(encoder.output.bytes);
}

fc::Result<fc::lua::Value> decode_value(fc::lua::Frame &frame, fc::Bytes bytes) {
    Decoder decoder{frame, {bytes}};
    auto value = decoder.value(0);
    auto status = frame.result();
    if (!status) {
        return std::unexpected(std::move(status.error()));
    }
    if (!value) {
        return value;
    }
    if (!decoder.input.remaining().empty()) {
        return std::unexpected(fc::Error{"Lua message value has trailing bytes"});
    }
    return value;
}

} // namespace fc::core_plugin::lua_multiplayer::protocol
