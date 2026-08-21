#pragma once

#include "catalog_types.hpp"

#include <expected>
#include <string>

namespace fc::catalog {

// Deep-copies the complete pointer graph synchronously under the per-plugin metadata byte budget.
[[nodiscard]] std::expected<PluginDefinitionRecord, std::string>
copy_plugin_definition(const FC_PluginDefinition* plugin);

// Applies target-independent structure rules while the contribution's callback code owner is still alive.
[[nodiscard]] std::expected<void, std::string>
validate_plugin_definition(const PluginDefinitionRecord& plugin, PluginOrigin origin, const CodeOwner& code_owner);

// Shared identity and text helpers keep admission and configuration matching on the same normalization rules.
[[nodiscard]] bool equal_ascii_case_insensitive(std::string_view left, std::string_view right) noexcept;
[[nodiscard]] std::string fold_ascii(std::string_view value);
[[nodiscard]] bool valid_utf8(std::string_view value) noexcept;
[[nodiscard]] bool valid_framework_id(std::string_view value) noexcept;

} // namespace fc::catalog
