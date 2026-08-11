#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <utility>

namespace fusioncutter {

enum class TargetLayout {
    SteamRetail,
    GOGRetail,
    Aspyr,
    ModTools,
};

enum class HostRole {
    Client,
    Server,
};

enum class TargetImage {
    Game,
    Bootstrap,
    GalaxyPeer,
};

enum class Architecture {
    X86,
    X64,
};

[[nodiscard]] constexpr Architecture target_architecture(TargetLayout layout) noexcept {
    switch (layout) {
    case TargetLayout::SteamRetail:
    case TargetLayout::GOGRetail:
    case TargetLayout::ModTools:
        return Architecture::X86;
    case TargetLayout::Aspyr:
        return Architecture::X64;
    }
    std::unreachable();
}

struct ImageContext {
    TargetImage identity;
    Architecture architecture;
    std::uintptr_t base;
    std::size_t size;

    [[nodiscard]] constexpr bool contains_rva(std::uint32_t rva, std::size_t extent) const noexcept {
        const auto offset = static_cast<std::size_t>(rva);
        return offset <= size && extent <= size - offset;
    }

    [[nodiscard]] constexpr std::uintptr_t address_at_rva(std::uint32_t rva, std::size_t extent = 1) const noexcept {
        const auto offset = static_cast<std::uintptr_t>(rva);
        if (base == 0 || !contains_rva(rva, extent) || base > std::numeric_limits<std::uintptr_t>::max() - offset) {
            return 0;
        }
        return base + offset;
    }

    template <typename Function>
        requires(std::is_pointer_v<Function> && std::is_function_v<std::remove_pointer_t<Function>>)
    [[nodiscard]] Function function_at_rva(std::uint32_t rva) const noexcept {
        const auto address = address_at_rva(rva);
        return address == 0 ? nullptr : reinterpret_cast<Function>(address);
    }

    template <typename T>
        requires(std::is_object_v<T> && !std::is_void_v<T>)
    // Resolves mutable game-owned runtime state; installation writes still belong to PatchPlan.
    [[nodiscard]] T* mutable_at_rva(std::uint32_t rva) const noexcept {
        const auto address = address_at_rva(rva, sizeof(T));
        if (address == 0) {
            return nullptr;
        }
        if (address % alignof(T) != 0) {
            return nullptr;
        }

        return reinterpret_cast<T*>(address);
    }

    template <typename T>
        requires(std::is_object_v<T> && !std::is_void_v<T>)
    [[nodiscard]] const T* read_at_rva(std::uint32_t rva) const noexcept {
        return mutable_at_rva<T>(rva);
    }
};

struct TargetContext {
    TargetLayout layout;
    HostRole role;
    ImageContext image;
};

} // namespace fusioncutter
