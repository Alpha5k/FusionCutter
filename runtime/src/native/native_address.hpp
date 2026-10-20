#pragma once

#include <fc/abi.h>

#include <cstdint>
#include <optional>

namespace fc::planning {

// Decodes a signed four-byte displacement from next, the address immediately after its field.
// x86 wraps at 32 bits; x64 returns nullopt if adding the displacement overflows the address space.
std::optional<std::uintptr_t> decode_rel32_target(std::uintptr_t next, std::int32_t displacement, FC_Architecture architecture) noexcept;

// Encodes target relative to next, the address after the displacement field, as a signed four-byte value.
// x86 preserves wrapping address arithmetic; x64 returns nullopt if the signed distance is out of range.
std::optional<std::int32_t> encode_rel32(std::uintptr_t next, std::uintptr_t target, FC_Architecture architecture) noexcept;

} // namespace fc::planning
