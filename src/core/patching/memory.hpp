#pragma once

#include "FusionCutter/outcome.hpp"
#include "FusionCutter/patching.hpp"
#include "FusionCutter/target.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>

namespace fusioncutter::patching_detail {

[[nodiscard]] bool readable_memory(std::uintptr_t address, std::size_t size) noexcept;

struct WriteFailure {
    std::string message;
    bool memory_changed;
};

[[nodiscard]] std::expected<void, WriteFailure> write_memory(std::uintptr_t address, std::span<const std::byte> bytes);

class DataAllocation {
  public:
    DataAllocation() = default;
    DataAllocation(const DataAllocation&) = delete;
    DataAllocation(DataAllocation&& other) noexcept;
    DataAllocation& operator=(const DataAllocation&) = delete;
    DataAllocation& operator=(DataAllocation&& other) noexcept;
    ~DataAllocation();

    [[nodiscard]] static std::expected<DataAllocation, std::string>
    create(std::size_t size, std::size_t alignment, const ImageContext& image, const AllocationProximity* proximity);

    [[nodiscard]] std::uintptr_t address() const noexcept {
        return address_;
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return size_;
    }

  private:
    void reset() noexcept;

    void* allocation_{};
    std::uintptr_t address_{};
    std::size_t size_{};
};

} // namespace fusioncutter::patching_detail
