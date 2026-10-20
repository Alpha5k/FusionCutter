#pragma once

#include <fc/abi.h>
#include <fc/base.hpp>

#include <expected>
#include <vector>

namespace fc::runtime {

// Owns a native call's argument descriptions independently of the submitted ABI record.
struct CallDescription {
    FC_NativeCall value{};
    std::vector<FC_NativeArgument> arguments;

    // Rebinds the ABI argument pointer to this object's storage after a copy or move.
    FC_NativeCall view() const noexcept;
};

// Copies and validates a call; bridge requests also require a supported physical register and stack layout.
std::expected<CallDescription, Error> validate_call(FC_NativeCall, bool bridge);
// Compares logical values, physical storage and cleanup rules required to share a hook site.
bool equivalent_call(const CallDescription &, const CallDescription &) noexcept;

} // namespace fc::runtime
