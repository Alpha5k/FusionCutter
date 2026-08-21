#include "runtime/core_runtime.hpp"

#include <new>
#include <utility>

namespace {

// Production intentionally leaks this owner so installed callbacks, native resources, and external DLLs remain valid.
[[nodiscard]] fc::runtime::CoreRuntime* runtime() noexcept {
    static auto* instance = new (std::nothrow) fc::runtime::CoreRuntime;
    return instance;
}

// Contains malformed loader input and exceptions while preserving the first call's authoritative result.
FC_InitializeResult FC_CALL initialize(const FC_InitializeArgs* arguments) noexcept {
    auto* owner = runtime();
    if (owner == nullptr) {
        return FC_INIT_FATAL;
    }
    if (const auto result = owner->initialization_result()) {
        return static_cast<FC_InitializeResult>(*result);
    }
    try {
        auto request = fc::runtime::copy_initialization_request(arguments);
        if (!request) {
            return static_cast<FC_InitializeResult>(owner->reject_initialization(std::move(request.error())));
        }
        return static_cast<FC_InitializeResult>(owner->initialize(*request));
    } catch (...) {
        return static_cast<FC_InitializeResult>(
            owner->reject_initialization("Fusion Cutter initialization ended unexpectedly"));
    }
}

// The exported Update callback is inert before a Completed result and delegates lifecycle ownership to CoreRuntime.
void FC_CALL update() noexcept {
    if (auto* owner = runtime()) {
        owner->update();
    }
}

// Query returns this immutable DLL-owned prefix for the lifetime of the retained FusionCutter.dll module.
const FC_CoreApi core_api{.struct_size = sizeof(FC_CoreApi), .initialize = &initialize, .update = &update};

} // namespace

// Unsupported ABI generations fail without constructing or touching the process-lifetime runtime owner.
FC_EXTERN_C const FC_CoreApi* FC_CALL FusionCutter_QueryCore(uint32_t abi_generation) FC_NOEXCEPT {
    return abi_generation == FC_CORE_ABI_GENERATION ? &core_api : nullptr;
}
