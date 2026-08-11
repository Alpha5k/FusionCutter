#pragma once

#include <atomic>
#include <cstdint>

namespace fusioncutter::patches::direct_transport {

// Claims and enforces the one game network thread allowed to mutate transport state.
class NetworkThreadAffinity {
  public:
    // Assigns the first caller as owner and rejects later calls from other threads.
    [[nodiscard]] bool claim_current() noexcept;
    // Checks the current caller against the established owner.
    [[nodiscard]] bool is_current() noexcept;
    [[nodiscard]] std::uint64_t rejected_calls() const noexcept;

  private:
    [[nodiscard]] bool matches(std::uint32_t current, std::uint32_t owner) noexcept;

    std::atomic_uint32_t owner_{};
    std::atomic_uint64_t rejected_calls_{};
};

} // namespace fusioncutter::patches::direct_transport
