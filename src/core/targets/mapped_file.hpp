#pragma once

#include <cstddef>
#include <expected>
#include <filesystem>
#include <string>
#include <vector>

namespace fc::targets {

// Maps a read-only PE file into a private copy laid out like a loaded image, then applies base relocations.
[[nodiscard]] std::expected<std::vector<std::byte>, std::string> map_pe_file(const std::filesystem::path& path);

} // namespace fc::targets
