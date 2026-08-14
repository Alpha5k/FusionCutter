#include "pipeline.hpp"

#include "layout.hpp"

#include <array>
#include <atomic>

namespace fusioncutter::patches::soldier_state_pipeline {
namespace {

using ReadFunction = void(__thiscall*)(void*, void*, std::uint32_t);

constexpr std::size_t kObserverCount = 2;

std::array<std::atomic<const ObserverCallbacks*>, kObserverCount> gObservers;
OriginalFunction<ReadFunction> gReadOriginal;

void __fastcall hook_read(void* soldier, void*, void* stream, std::uint32_t flags) noexcept {
    const ReadContext context{soldier, stream, flags};
    std::array<const ObserverCallbacks*, kObserverCount> observers{};
    std::array<ObserverState, kObserverCount> states{};

    for (std::size_t index{}; index < observers.size(); ++index) {
        observers[index] = gObservers[index].load(std::memory_order_acquire);
        if (observers[index] != nullptr) {
            observers[index]->before_read(observers[index]->context, context, states[index]);
        }
    }

    if (const auto original = gReadOriginal.get(); original != nullptr) {
        original(soldier, stream, flags);
    }

    for (std::size_t index{}; index < observers.size(); ++index) {
        if (observers[index] != nullptr) {
            observers[index]->after_read(observers[index]->context, context, states[index]);
        }
    }
}

} // namespace

SoldierStatePipeline::SoldierStatePipeline(const TargetContext& target) noexcept : target_(target) {}

void SoldierStatePipeline::build_plan(PatchPlan& plan) {
    const auto& layout = layout_for(target_.layout);
    gReadOriginal =
        plan.inline_hook_with_original<ReadFunction>("Share Soldier state reads", layout.read.rva,
                                                     layout.read.pattern(), reinterpret_cast<ReadFunction>(&hook_read));
}

void publish_observer(const ObserverCallbacks& callbacks) noexcept {
    for (auto& slot : gObservers) {
        auto* expected = static_cast<const ObserverCallbacks*>(nullptr);
        if (slot.compare_exchange_strong(expected, &callbacks, std::memory_order_acq_rel)) {
            return;
        }
        if (expected == &callbacks) {
            return;
        }
    }
}

void clear_observer(const ObserverCallbacks& callbacks) noexcept {
    for (auto& slot : gObservers) {
        auto* expected = &callbacks;
        static_cast<void>(slot.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel));
    }
}

} // namespace fusioncutter::patches::soldier_state_pipeline
