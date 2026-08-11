#pragma once

#include "FusionCutter/outcome.hpp"
#include "FusionCutter/target.hpp"
#include "plan_storage.hpp"

#include <safetyhook/inline_hook.hpp>
#include <safetyhook/mid_hook.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fusioncutter::patching_detail {

[[nodiscard]] OutcomeReason operation_failure(std::string message, std::string_view operation = {});
[[nodiscard]] bool valid_pattern(const OwnedPattern& pattern) noexcept;
[[nodiscard]] bool pattern_matches(std::uintptr_t address, const OwnedPattern& pattern) noexcept;
[[nodiscard]] std::expected<std::uintptr_t, std::string> target_address(const ImageContext& image, std::uint32_t rva,
                                                                        std::size_t size);
[[nodiscard]] std::string inline_hook_error(const safetyhook::InlineHook::Error& error);
[[nodiscard]] std::string mid_hook_error(const safetyhook::MidHook::Error& error);
[[nodiscard]] std::vector<std::byte> copy_bytes(std::uintptr_t address, std::size_t size);
[[nodiscard]] bool equal_memory(std::uintptr_t address, std::span<const std::byte> expected) noexcept;

} // namespace fusioncutter::patching_detail
