#pragma once

#include <FusionCutter/Abi.h>

#include <cstdint>
#include <optional>

namespace fc::planning {

// Decodes the architecture-specific wrapping or signed displacement used by a native rel32 field.
[[nodiscard]] std::optional<std::uintptr_t> decode_rel32_target(std::uintptr_t next, std::int32_t displacement,
                                                                FC_Architecture architecture) noexcept;

// Reports whether a target can be encoded directly from the address immediately following a rel32 field.
[[nodiscard]] bool rel32_reachable(std::uintptr_t next, std::uintptr_t target, FC_Architecture architecture) noexcept;

// Produces the exact four displacement bytes after reachability has been established.
[[nodiscard]] std::optional<std::int32_t> encode_rel32(std::uintptr_t next, std::uintptr_t target,
                                                       FC_Architecture architecture) noexcept;

} // namespace fc::planning
