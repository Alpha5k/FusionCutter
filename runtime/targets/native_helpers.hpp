#pragma once

#include <fc/native.hpp>

#include <cstdint>
#include <expected>
#include <initializer_list>

namespace fc::runtime::targets {

// Builds exact instruction bytes using the loaded address for an absolute operand.
inline fc::Evidence reference(std::uintptr_t image_base, std::initializer_list<std::uint8_t> prefix, std::uint32_t rva,
                              std::initializer_list<std::uint8_t> suffix = {}) {
    auto evidence = fc::exact_bytes(prefix);
    const auto address = image_base + rva;
    for (unsigned shift = 0; shift < 32; shift += 8) {
        evidence.bytes.push_back(static_cast<std::uint8_t>(address >> shift));
    }
    evidence.bytes.insert(evidence.bytes.end(), suffix);
    return evidence;
}

// Stores the prepared memory view for direct reads in native callbacks.
template <class T> fc::Result<> retain_view(const fc::Memory<T> &memory, T *&pointer) {
    const auto view = memory.view();
    if (!view) {
        return std::unexpected(view.error());
    }

    pointer = view->data();
    return {};
}

} // namespace fc::runtime::targets
