#pragma once

#include <FusionCutter/PluginApi.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace fc::patching {

// Constrains an allocation address to the displacement range of the native instruction that will reference it.
struct NearConstraint {
    std::uintptr_t reference{};
    std::size_t maximum_distance{};
};

// Owns one aligned VirtualAlloc reservation and releases it only while the allocation remains unexposed.
class NativeAllocation final {
  public:
    NativeAllocation() = default;
    NativeAllocation(const NativeAllocation&) = delete;
    NativeAllocation& operator=(const NativeAllocation&) = delete;
    NativeAllocation(NativeAllocation&& other) noexcept;
    NativeAllocation& operator=(NativeAllocation&& other) noexcept;
    ~NativeAllocation();

    [[nodiscard]] static std::expected<NativeAllocation, std::string>
    create(std::size_t byte_size, std::size_t alignment, std::optional<NearConstraint> constraint = std::nullopt);

    // Initialization fills unpublished storage; making a relay executable closes its writable preparation window.
    [[nodiscard]] std::expected<void, std::string> initialize(std::span<const std::byte> bytes);
    [[nodiscard]] std::expected<void, std::string> make_executable();

    [[nodiscard]] std::uintptr_t address() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;

  private:
    void reset() noexcept;

    void* reservation_{};
    std::size_t reservation_size_{};
    std::uintptr_t address_{};
    std::size_t size_{};
};

// The symbolic handle travels with its physical allocation until installed or retained failure ownership takes over.
struct NativeDataAllocation {
    FC_DataHandle handle{};
    NativeAllocation allocation;
};

// Installed or retained owners keep all storage that native writes may reference for the rest of the process.
struct NativePatchResources {
    std::vector<NativeDataAllocation> data_allocations;
    std::vector<NativeAllocation> relay_allocations;
};

} // namespace fc::patching
