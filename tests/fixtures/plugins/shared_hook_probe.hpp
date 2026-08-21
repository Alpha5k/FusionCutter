#pragma once

#include <FusionCutter/PluginApi.h>

#include <Windows.h>

#include <cstdint>

// The prototype exchanges only this C-compatible state; callbacks and builders remain owned by separate DLLs.
struct FcHookProbeOwnerContext {
    std::uintptr_t original{};
    std::uintptr_t nested_entry{};
    std::int32_t adjustment{};
    volatile LONG calls{};
};

struct FcHookProbeObserverContext {
    volatile LONG before_calls{};
    volatile LONG after_calls{};
    volatile LONG state_mismatches{};
};

// A twelve-byte POD forces the compiler-facing SDK boundary to use a hidden result on both supported architectures.
struct FcHookProbeRecord {
    std::int32_t left{};
    std::int32_t right{};
    std::int32_t total{};
};

using FcHookProbeCall = std::int32_t(FC_CALL*)(std::int32_t left, std::int32_t right) noexcept;
