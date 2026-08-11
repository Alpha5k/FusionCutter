#include "thread_affinity.hpp"

#include <Windows.h>

namespace fusioncutter::patches::direct_transport {

bool NetworkThreadAffinity::claim_current() noexcept {
    const auto current = GetCurrentThreadId();
    auto owner = std::uint32_t{};
    if (owner_.compare_exchange_strong(owner, current, std::memory_order_acq_rel, std::memory_order_acquire)) {
        return true;
    }
    return matches(current, owner);
}

bool NetworkThreadAffinity::is_current() noexcept {
    const auto owner = owner_.load(std::memory_order_acquire);
    return owner == 0 || matches(GetCurrentThreadId(), owner);
}

std::uint64_t NetworkThreadAffinity::rejected_calls() const noexcept {
    return rejected_calls_.load(std::memory_order_acquire);
}

bool NetworkThreadAffinity::matches(std::uint32_t current, std::uint32_t owner) noexcept {
    if (current == owner) {
        return true;
    }
    rejected_calls_.fetch_add(1, std::memory_order_relaxed);
    return false;
}

} // namespace fusioncutter::patches::direct_transport
