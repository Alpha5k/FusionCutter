#pragma once

#include "simple_write_constants.h"

#include <cstdint>
#include <string_view>

namespace fc::fixtures::simple_write {

inline constexpr std::string_view kPluginId = "SimpleWriteSlice";
inline constexpr std::string_view kPatchId = "SimpleCheckedWrite";
inline constexpr std::uint32_t kInitialValue = FC_SIMPLE_WRITE_INITIAL_VALUE;
inline constexpr std::uint32_t kDefaultReplacement = FC_SIMPLE_WRITE_DEFAULT_REPLACEMENT;
inline constexpr std::uint32_t kOverrideReplacement = FC_SIMPLE_WRITE_OVERRIDE_REPLACEMENT;

} // namespace fc::fixtures::simple_write
