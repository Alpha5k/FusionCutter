#pragma once

#include "diagnostic.hpp"

#include <fc/abi.h>

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace fc::runtime {

// Owns a parsed scalar, including text retained for later handler construction.
struct ScalarValue {
    std::variant<bool, std::int64_t, std::uint64_t, double, std::string> value;

    // Text in the returned C value borrows this object's storage and is valid until that storage changes.
    FC_Value view() const;
};

// Owns one explicit assignment after whitespace trimming and optional JSON string decoding.
struct IniEntry {
    std::string section, key, value;
};

// Owns parsed assignments and identities used to capture settings and generate missing defaults.
struct IniDocument {
    std::vector<IniEntry> entries;
    // Retains every section occurrence, including repeats and sections with no assignments, for warning checks.
    std::vector<std::string> sections;
    // Folded section/key identities of unset commented assignments, used to avoid appending them again.
    std::vector<std::string> commented_keys;
};

namespace configuration_detail {

// Removes surrounding space, tab and carriage return; the result borrows the input bytes.
std::string_view trim(std::string_view);

// Builds an ASCII case-insensitive section/key identity with a separating newline.
std::string identity(std::string_view section, std::string_view key);

// Returns an explicit assignment by ASCII case-insensitive identity, or nullptr; the document owns the result.
const IniEntry *find(const IniDocument &, std::string_view section, std::string_view key);

} // namespace configuration_detail

// Parses borrowed UTF-8 input into owned assignments, trimming unquoted values and decoding JSON strings; repeated sections merge.
// Malformed UTF-8, structure or duplicate ASCII case-insensitive section/key pairs return a diagnostic.
std::expected<IniDocument, Diagnostic> parse_ini(std::string_view content, std::string_view owner);

// Converts decoded text to the requested FC_SCALAR kind; numbers must consume all input and real values must be finite.
// Booleans accept ASCII case-insensitive true/false; text must be UTF-8 and may include null characters.
std::expected<ScalarValue, Diagnostic> parse_scalar(std::string_view value, std::uint32_t kind, std::string_view owner);

// Produces INI text for a scalar; strings use JSON quotes and escapes, and doubles use round-trip precision.
std::string format_scalar(const FC_Value &value);

} // namespace fc::runtime
