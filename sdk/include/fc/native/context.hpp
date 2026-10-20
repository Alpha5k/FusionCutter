#pragma once

#include <cstdint>

namespace fc {

// Describes the native call being observed or replaced. Typed callbacks may accept it before their ordinary arguments.
class NativeContext {
  public:
    explicit NativeContext(std::uintptr_t return_address = 0) noexcept : return_address_(return_address) {}

    // Returns the original caller's return address, or zero for instruction callbacks and runtimes without this facility.
    std::uintptr_t return_address() const noexcept {
        return return_address_;
    }

  private:
    std::uintptr_t return_address_{};
};

} // namespace fc
