#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>

namespace fc::reporting {

// Stores variable-sized trace records for concurrent producers and one consumer in a fixed byte budget.
class ByteRing {
  public:
    struct Reservation {
        std::uint64_t position{};
        std::uint32_t bytes{};
    };

    ByteRing() = default;

    // Allocates storage once before use; framing and eight-byte alignment fit within the declared byte budget.
    void initialize(std::size_t bytes) {
        capacity = bytes;
        usable_ = bytes - bytes % sizeof(Cell);
        storage_ = std::make_unique<Cell[]>(usable_ / sizeof(Cell));
    }

    // Reserves a complete record without waiting for space; contention retries only the atomic reservation.
    [[nodiscard]] std::optional<Reservation> reserve(std::uint32_t bytes) noexcept {
        const auto charge =
            (static_cast<std::uint64_t>(bytes) + sizeof(std::uint32_t) + sizeof(Cell) - 1) & ~std::uint64_t{sizeof(Cell) - 1};
        if (charge > usable_) {
            return std::nullopt;
        }

        auto tail = tail_.load(std::memory_order_relaxed);
        for (;;) {
            // Acquire the consumer's clearing before reserving storage that previously held another record.
            const auto head = head_.load(std::memory_order_acquire);
            if (charge > usable_ - (tail - head)) {
                return std::nullopt;
            }
            if (tail_.compare_exchange_weak(tail, tail + charge, std::memory_order_relaxed)) {
                return Reservation{tail, static_cast<std::uint32_t>(charge)};
            }
        }
    }

    // Copies bytes into this producer's reservation; offset and size must stay within its unframed record.
    void copy_in(Reservation record, std::size_t offset, const void *source, std::size_t size) noexcept {
        const auto start = static_cast<std::size_t>((record.position % usable_ + sizeof(std::uint32_t) + offset) % usable_);
        const auto first = std::min(size, usable_ - start);
        if (first) {
            std::memcpy(data() + start, source, first);
        }
        if (size > first) {
            std::memcpy(data(), static_cast<const std::byte *>(source) + first, size - first);
        }
    }

    // Publishes only after all record bytes have been copied; the caller then wakes the consumer.
    void publish(Reservation record) noexcept {
        header(record.position).store(record.bytes, std::memory_order_release);
    }

    // Returns the first complete record; an empty or unpublished head ends this consumer pass immediately.
    std::optional<Reservation> next() noexcept {
        if (!usable_) {
            return std::nullopt;
        }
        const auto head = head_.load(std::memory_order_relaxed);
        const auto bytes = header(head).load(std::memory_order_acquire);
        return bytes ? std::optional{Reservation{head, bytes}} : std::nullopt;
    }

    // Copies published bytes to consumer storage before release; offset and size must stay within the record.
    void copy_out(Reservation record, std::size_t offset, void *destination, std::size_t size) noexcept {
        const auto start = static_cast<std::size_t>((record.position % usable_ + sizeof(std::uint32_t) + offset) % usable_);
        const auto first = std::min(size, usable_ - start);
        if (first) {
            std::memcpy(destination, data() + start, first);
        }
        if (size > first) {
            std::memcpy(static_cast<std::byte *>(destination) + first, data(), size - first);
        }
    }

    // Clears a consumed record before returning its capacity; only the consumer calls this operation.
    void release(Reservation record) noexcept {
        const auto start = static_cast<std::size_t>(record.position % usable_);
        header(record.position).store(0, std::memory_order_relaxed);
        // Clear payload and padding too: their words can become publication headers after the ring wraps.
        const auto remaining = static_cast<std::size_t>(record.bytes) - sizeof(std::uint32_t);
        const auto body = start + sizeof(std::uint32_t);
        const auto first = std::min(remaining, usable_ - body);
        std::memset(data() + body, 0, first);
        if (remaining > first) {
            std::memset(data(), 0, remaining - first);
        }
        head_.store(record.position + record.bytes, std::memory_order_release);
    }

    // Reclaims an empty disabled channel after all producers have left; only the consumer may call this.
    void reclaim() noexcept {
        storage_.reset();
        usable_ = 0;
    }

    // Original author-selected byte budget, retained in channel metadata after reclamation.
    std::size_t capacity{};

  private:
    // Every potential header has a live, aligned uint32_t object; other words hold copied object representations.
    struct alignas(8) Cell {
        std::uint32_t words[2]{};
    };

    static_assert(std::atomic_ref<std::uint32_t>::required_alignment <= alignof(Cell));
    static_assert(std::atomic_ref<std::uint32_t>::is_always_lock_free);
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

    // Views cell object representations as byte storage for split copies.
    std::byte *data() noexcept {
        return reinterpret_cast<std::byte *>(storage_.get());
    }

    // Accesses only aligned first words; atomic_ref lifetimes never overlap ordinary access to the same word.
    std::atomic_ref<std::uint32_t> header(std::uint64_t position) noexcept {
        return std::atomic_ref{storage_[static_cast<std::size_t>(position % usable_) / sizeof(Cell)].words[0]};
    }

    std::unique_ptr<Cell[]> storage_;
    std::size_t usable_{};
    // Monotonic byte positions assume less than 2^64 bytes pass through this queue during its lifetime.
    alignas(8) std::atomic<std::uint64_t> head_{}, tail_{};
};

} // namespace fc::reporting
