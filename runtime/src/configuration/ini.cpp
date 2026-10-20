#include "ini.hpp"

#include <fc/base.hpp>

#include <Windows.h>

#include <algorithm>
#include <charconv>
#include <climits>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <unordered_set>

namespace fc::runtime {

namespace configuration_detail {

std::string_view trim(std::string_view value) {
    const auto first = value.find_first_not_of(" \t\r");
    if (first == std::string_view::npos) {
        return {};
    }
    return value.substr(first, value.find_last_not_of(" \t\r") - first + 1);
}

std::string identity(std::string_view section, std::string_view key) {
    return fc::detail::folded_id(section) + "\n" + fc::detail::folded_id(key);
}

const IniEntry *find(const IniDocument &document, std::string_view section, std::string_view key) {
    for (const auto &entry : document.entries) {
        if (fc::detail::equal_id(entry.section, section) && fc::detail::equal_id(entry.key, key)) {
            return &entry;
        }
    }
    return nullptr;
}

} // namespace configuration_detail

namespace {

// Validate UTF-8 with Windows; INI source rejects null bytes while decoded scalar text may contain them.
bool utf8(std::string_view value, bool allow_null = false) {
    if ((!allow_null && value.find('\0') != std::string_view::npos) || value.size() > INT_MAX) {
        return false;
    }
    return value.empty() ||
           MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0) != 0;
}

// Decode a quoted JSON string; unquoted values retain literal comment characters and backslashes.
std::expected<std::string, Diagnostic> decode_string(std::string_view value, std::string_view owner) {
    if (!value.starts_with('"')) {
        return std::string{value};
    }

    std::string result;
    std::size_t i = 1;
    auto fail = [&] {
        return std::unexpected(Diagnostic{std::string{owner}, "Malformed JSON string"});
    };

    // Each JSON Unicode escape contributes exactly four hexadecimal digits.
    auto hex = [&](std::uint32_t &code) {
        if (value.size() - i < 4) {
            return false;
        }
        code = 0;
        for (int n = 0; n < 4; ++n) {
            const char c = value[i++];
            const int digit = c >= '0' && c <= '9'   ? c - '0'
                              : c >= 'a' && c <= 'f' ? c - 'a' + 10
                              : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                                     : -1;
            if (digit < 0) {
                return false;
            }
            code = code * 16 + static_cast<std::uint32_t>(digit);
        }
        return true;
    };

    // A closing quote must finish the value; unescaped control bytes and incomplete escapes are invalid.
    while (i < value.size()) {
        const auto c = static_cast<unsigned char>(value[i++]);
        if (c == '"') {
            if (i != value.size()) {
                return fail();
            }
            return result;
        }
        if (c < 0x20) {
            return fail();
        }
        if (c != '\\') {
            result += static_cast<char>(c);
            continue;
        }
        if (i == value.size()) {
            return fail();
        }

        switch (value[i++]) {
        case '"':
            result += '"';
            break;
        case '\\':
            result += '\\';
            break;
        case '/':
            result += '/';
            break;
        case 'b':
            result += '\b';
            break;
        case 'f':
            result += '\f';
            break;
        case 'n':
            result += '\n';
            break;
        case 'r':
            result += '\r';
            break;
        case 't':
            result += '\t';
            break;
        case 'u': {
            std::uint32_t code{};
            if (!hex(code)) {
                return fail();
            }
            // Combine a UTF-16 high/low surrogate pair into one code point and reject unmatched surrogates.
            if (code >= 0xd800 && code <= 0xdbff) {
                if (value.size() - i < 6 || value.substr(i, 2) != "\\u") {
                    return fail();
                }
                i += 2;
                std::uint32_t low{};
                if (!hex(low) || low < 0xdc00 || low > 0xdfff) {
                    return fail();
                }
                code = 0x10000 + ((code - 0xd800) << 10) + low - 0xdc00;
            } else if (code >= 0xdc00 && code <= 0xdfff) {
                return fail();
            }

            // Encode the decoded Unicode code point as one to four UTF-8 bytes.
            if (code <= 0x7f) {
                result += static_cast<char>(code);
            } else if (code <= 0x7ff) {
                result += static_cast<char>(0xc0 | (code >> 6));
                result += static_cast<char>(0x80 | (code & 63));
            } else if (code <= 0xffff) {
                result += static_cast<char>(0xe0 | (code >> 12));
                result += static_cast<char>(0x80 | ((code >> 6) & 63));
                result += static_cast<char>(0x80 | (code & 63));
            } else {
                result += static_cast<char>(0xf0 | (code >> 18));
                result += static_cast<char>(0x80 | ((code >> 12) & 63));
                result += static_cast<char>(0x80 | ((code >> 6) & 63));
                result += static_cast<char>(0x80 | (code & 63));
            }
            break;
        }
        default:
            return fail();
        }
    }

    return fail();
}

// Escape JSON delimiters and control bytes while retaining ordinary UTF-8 bytes as written.
std::string quote(std::string_view value) {
    std::string output = "\"";
    constexpr char digits[] = "0123456789abcdef";
    for (const auto c : value) {
        if (c == '"' || c == '\\') {
            output += '\\';
            output += c;
        } else if (static_cast<unsigned char>(c) < 32) {
            output += "\\u00";
            output += digits[(static_cast<unsigned char>(c) >> 4) & 15];
            output += digits[static_cast<unsigned char>(c) & 15];
        } else {
            output += c;
        }
    }

    return output + '"';
}

} // namespace

using configuration_detail::identity;
using configuration_detail::trim;

FC_Value ScalarValue::view() const {
    return std::visit(
        [](const auto &input) {
            return fc::detail::scalar(input);
        },
        value);
}

std::expected<IniDocument, Diagnostic> parse_ini(std::string_view content, std::string_view owner) {
    if (!utf8(content)) {
        return std::unexpected(
            Diagnostic{std::string{owner},
                       "Configuration contains null bytes, is too large for Windows text conversion, or could not be validated as UTF-8"});
    }

    // Parse borrowed line views while retaining owned section/key/value strings in the result.
    IniDocument document;
    std::unordered_set<std::string> keys;
    std::string section;
    std::string_view remaining{content};
    std::size_t line{};
    while (!remaining.empty()) {
        ++line;
        const auto end = remaining.find('\n');
        auto value = trim(remaining.substr(0, end));
        remaining.remove_prefix(end == std::string_view::npos ? remaining.size() : end + 1);
        if (value.empty()) {
            continue;
        }

        if (value.front() == ';' || value.front() == '#') {
            // Unset commented assignments document variant defaults without becoming input overrides.
            const auto comment = trim(value.substr(1));
            const auto separator = comment.find('=');
            if (!section.empty() && separator != std::string_view::npos && trim(comment.substr(separator + 1)).empty()) {
                const auto key = trim(comment.substr(0, separator));
                if (!key.empty()) {
                    document.commented_keys.push_back(identity(section, key));
                }
            }
            continue;
        }

        auto fail = [&](std::string message) {
            return std::unexpected(Diagnostic{std::string{owner}, "Line " + std::to_string(line) + ": " + message});
        };
        // Repeated sections select the same identity; every occurrence remains available for warning checks.
        if (value.front() == '[') {
            if (value.size() < 3 || value.back() != ']' ||
                value.substr(1, value.size() - 2).find_first_of("[]") != std::string_view::npos) {
                return fail("Malformed section header");
            }
            section = trim(value.substr(1, value.size() - 2));
            if (section.empty()) {
                return fail("Section name is empty");
            }
            document.sections.push_back(section);
            continue;
        }

        // Split at the first assignment separator and reject duplicate ASCII case-insensitive section/key pairs.
        const auto separator = value.find('=');
        if (section.empty() || separator == std::string_view::npos || trim(value.substr(0, separator)).empty()) {
            return fail("Expected a section header and a key=value assignment");
        }
        auto key = trim(value.substr(0, separator));
        if (key.find_first_of("[]") != std::string_view::npos) {
            return fail("Key contains '[' or ']'");
        }
        if (!keys.insert(identity(section, key)).second) {
            return fail("Duplicate key [" + section + "]." + std::string{key});
        }

        // Decode only the value; structural and string errors report the original file's line number.
        auto decoded = decode_string(trim(value.substr(separator + 1)), owner);
        if (!decoded) {
            return fail(decoded.error().message);
        }
        document.entries.push_back({section, std::string{key}, std::move(*decoded)});
    }

    return document;
}

std::expected<ScalarValue, Diagnostic> parse_scalar(std::string_view value, std::uint32_t kind, std::string_view owner) {
    auto fail = [&] {
        return std::unexpected(Diagnostic{std::string{owner}, "Value does not match the setting's declared type"});
    };

    // Text keeps decoded whitespace and null characters; booleans accept ASCII case-insensitive true and false.
    if (kind == FC_SCALAR_TEXT) {
        if (!utf8(value, true)) {
            return fail();
        }
        return ScalarValue{std::string{value}};
    }

    if (kind == FC_SCALAR_BOOL) {
        if (fc::detail::equal_id(value, "true")) {
            return ScalarValue{true};
        }
        if (fc::detail::equal_id(value, "false")) {
            return ScalarValue{false};
        }
        return fail();
    }

    // Numeric parsing must consume every byte; overflow, suffixes and non-finite real values reject the input.
    if (kind == FC_SCALAR_SIGNED) {
        std::int64_t result{};
        auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
        if (parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size()) {
            return ScalarValue{result};
        }
    } else if (kind == FC_SCALAR_UNSIGNED) {
        std::uint64_t result{};
        auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
        if (parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size()) {
            return ScalarValue{result};
        }
    } else if (kind == FC_SCALAR_REAL) {
        double result{};
        auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
        if (parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() && std::isfinite(result)) {
            return ScalarValue{result};
        }
    }

    return fail();
}

std::string format_scalar(const FC_Value &value) {
    switch (value.kind) {
    case FC_SCALAR_BOOL:
        return value.value.boolean ? "true" : "false";
    case FC_SCALAR_SIGNED:
        return std::to_string(value.value.signed_value);
    case FC_SCALAR_UNSIGNED:
        return std::to_string(value.value.unsigned_value);
    case FC_SCALAR_REAL: {
        std::ostringstream out;
        // Seventeen significant digits preserve a double through decimal serialization and parsing.
        out << std::setprecision(17) << value.value.real;
        return out.str();
    }
    default:
        return quote(fc::detail::view(value.value.text));
    }
}

} // namespace fc::runtime
