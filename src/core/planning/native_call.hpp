#pragma once

#include "planning_types.hpp"

#include <expected>
#include <string>

namespace fc::planning {

// Copies and validates one complete normalized call against the recognized target architecture.
[[nodiscard]] std::expected<NativeCallRecord, std::string> validate_native_call(const FC_NativeCall& call,
                                                                                FC_Architecture architecture);

// Compares normalized call contracts so independently submitted hook participants can safely share one site.
[[nodiscard]] bool equivalent_native_call(const NativeCallRecord& left, const NativeCallRecord& right) noexcept;

} // namespace fc::planning
