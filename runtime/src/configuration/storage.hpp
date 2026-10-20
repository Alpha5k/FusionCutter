#pragma once

#include "ini.hpp"

#include <filesystem>
#include <span>
#include <tuple>

namespace fc::runtime::configuration_detail {

// Describes a declared INI entry for schema checks and generation of missing defaults.
struct GeneratedEntry {
    // Section/key locate the assignment; description and constraints become generated INI comments.
    std::string section, key, description, constraints;
    // Text values borrow admitted declarations or caller storage for the duration of load_file.
    FC_Value value;
    // Differing variant defaults require an unset commented key so selection can choose its own default.
    bool variant_default{};
    // Presentation order: explicitly ordered categories, unordered categories, then other sections.
    std::tuple<std::uint32_t, std::uint32_t, std::string> rank;
    // ASCII-folded key used to sort toggles within a category.
    std::string key_id;
};

// Returns the original parsed input; missing files return an empty document when writes are disabled.
// Enabled writes add grouped defaults without parsing additions; read/parse/create errors fail, update errors warn.
std::expected<IniDocument, Diagnostic> load_file(const std::filesystem::path &, std::span<const GeneratedEntry>, bool write,
                                                 std::vector<Diagnostic> &warnings);

// Adds warnings for sections and keys absent from the applicable schema while leaving their text intact.
void warn_unknown(const IniDocument &, std::span<const GeneratedEntry>, std::vector<Diagnostic> &warnings, std::string_view owner);

} // namespace fc::runtime::configuration_detail
