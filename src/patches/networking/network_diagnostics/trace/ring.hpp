#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace fusioncutter::patches::network_diagnostics::trace {

// Exchanges bounded records between one game thread and the trace writer.
template <typename Value, std::size_t Capacity> class SpscRing {
    static_assert(Capacity > 1);

  public:
    [[nodiscard]] bool push(const Value& value) noexcept {
        const auto write = write_.value.load(std::memory_order_relaxed);
        const auto next = increment(write);
        if (next == read_.value.load(std::memory_order_acquire)) {
            return false;
        }
        values_[write] = value;
        write_.value.store(next, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool pop(Value& value) noexcept {
        const auto read = read_.value.load(std::memory_order_relaxed);
        if (read == write_.value.load(std::memory_order_acquire)) {
            return false;
        }
        value = values_[read];
        read_.value.store(increment(read), std::memory_order_release);
        return true;
    }

    [[nodiscard]] std::uint32_t size() const noexcept {
        const auto write = write_.value.load(std::memory_order_acquire);
        const auto read = read_.value.load(std::memory_order_acquire);
        return static_cast<std::uint32_t>(write >= read ? write - read : Capacity - read + write);
    }

  private:
    [[nodiscard]] static constexpr std::size_t increment(std::size_t value) noexcept {
        return value + 1 == Capacity ? 0 : value + 1;
    }

    struct alignas(64) Cursor {
        std::atomic<std::size_t> value{};
        std::array<std::byte, 64 - sizeof(value)> padding{};
    };

    std::array<Value, Capacity> values_{};
    Cursor write_;
    Cursor read_;
};

} // namespace fusioncutter::patches::network_diagnostics::trace
