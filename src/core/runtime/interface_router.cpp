#include "interface_router.hpp"

#include "../fatal_boundary.hpp"

#include "../catalog/definition_copy.hpp"

#include <array>
#include <cstring>
#include <exception>
#include <limits>
#include <ranges>
#include <string>
#include <utility>
#include <variant>

namespace fc::runtime {
namespace {

inline constexpr std::size_t kInterfaceByteCapacity = 512;

// Interface scratch must satisfy every author contract alignment allowed by the public SDK concept.
struct alignas(std::max_align_t) InterfaceScratch {
    std::array<std::byte, kInterfaceByteCapacity> bytes{};
};

// Gives every allocation failure while finalizing a route the same lifecycle and diagnostic attribution.
[[nodiscard]] planning::FailureReason route_failure(std::string message) {
    return {
        .message = std::move(message), .phase = planning::PatchPhase::Prepare, .operation = "Prepare interface routes"};
}

} // namespace

void InterfaceRouter::set_logger(CoreLogger logger) noexcept {
    logger_ = logger;
}

void InterfaceRouter::reserve(std::size_t provider_capacity, std::size_t binding_capacity) {
    providers_.reserve(provider_capacity);
    bindings_.reserve(binding_capacity);
}

std::expected<InterfacePreparation, planning::FailureReason>
InterfaceRouter::prepare(std::span<const planning::OperationRecord> operations) const {
    InterfacePreparation result;
    try {
        // Size the exact copied subset once so later activation only transfers already-owned binding records.
        const auto count = static_cast<std::size_t>(std::ranges::count_if(operations, [](const auto& operation) {
            return std::holds_alternative<planning::InterfaceBindingOperation>(operation.payload);
        }));
        result.bindings_.reserve(count);
        // Preserve Plan operation order for deterministic connection when a provider is already present.
        for (const auto& operation : operations) {
            if (const auto* binding = std::get_if<planning::InterfaceBindingOperation>(&operation.payload)) {
                result.bindings_.push_back(*binding);
            }
        }
    } catch (...) {
        return std::unexpected(route_failure("Interface route storage could not be prepared"));
    }
    return result;
}

const InterfaceRouter::ProviderRecord* InterfaceRouter::find_provider(std::string_view id,
                                                                      bool require_active) const noexcept {
    const auto found = std::ranges::find_if(providers_, [&](const ProviderRecord& provider) {
        return (!require_active || provider.active) && catalog::equal_ascii_case_insensitive(provider.id, id);
    });
    return found == providers_.end() ? nullptr : &*found;
}

bool InterfaceRouter::query(const ProviderRecord& provider, std::string_view id, std::uint32_t size,
                            void* output) const noexcept {
    // Reject malformed transport before a plugin can observe an invalid buffer or unbounded size.
    if (provider.query == nullptr || output == nullptr || size == 0 || size > kInterfaceByteCapacity) {
        return false;
    }

    InterfaceScratch scratch;
    FC_Bool result = FC_FALSE;
    // Providers write into aligned framework scratch; consumer storage remains untouched until acceptance.
    try {
        result = provider.query(provider.callback_context, provider.handle,
                                {id.data(), static_cast<std::uint32_t>(id.size())}, size, scratch.bytes.data());
    } catch (...) {
        // An exception crossing the declared native callback boundary is not a recoverable interface miss.
        fatal_invariant("An installed interface provider callback threw across the native boundary");
    }
    if (result == FC_FALSE) {
        return false;
    }
    // A noncanonical Boolean is diagnosed as a plugin ABI violation without changing the installed provider result.
    if (result != FC_TRUE) {
        if (noncanonical_results_ != std::numeric_limits<std::uint64_t>::max()) {
            ++noncanonical_results_;
        }
        logger_.warning("Installed provider '{}' returned a noncanonical Boolean for interface '{}'", provider.id, id);
        return false;
    }
    std::memcpy(output, scratch.bytes.data(), size);
    return true;
}

FC_Bool InterfaceRouter::find_active(std::string_view provider_patch, std::string_view id, std::uint32_t size,
                                     void* output) const noexcept {
    if (provider_patch.empty() || id.empty() || output == nullptr || size == 0 || size > kInterfaceByteCapacity) {
        return FC_FALSE;
    }
    // A miss returns deterministic zeroed consumer storage rather than leaking stale bytes from a previous lookup.
    std::memset(output, 0, size);
    const auto* provider = find_provider(provider_patch, true);
    return provider != nullptr && query(*provider, id, size, output) ? FC_TRUE : FC_FALSE;
}

void InterfaceRouter::try_connect(BindingRecord& binding, const ProviderRecord& provider) noexcept {
    if (binding.connected || !catalog::equal_ascii_case_insensitive(binding.operation.provider_patch, provider.id)) {
        return;
    }

    InterfaceScratch scratch;
    // Query and connect are one bounded delivery; an unavailable exact contract leaves the route inert forever.
    if (!query(provider, binding.operation.id, binding.operation.size, scratch.bytes.data())) {
        logger_.debug("Installed provider '{}' does not expose optional interface '{}'",
                      binding.operation.provider_patch, binding.operation.id);
        return;
    }
    // The SDK thunk copies from scratch before entering author code; neither side retains framework working storage.
    try {
        binding.operation.connect(binding.operation.context, scratch.bytes.data());
    } catch (...) {
        fatal_invariant("An installed interface consumer callback threw across the native boundary");
    }
    binding.connected = true;
    logger_.debug("Connected optional interface '{}' from provider '{}'", binding.operation.id,
                  binding.operation.provider_patch);
}

void InterfaceRouter::connect_consumer(catalog::PatchIndex consumer, InterfacePreparation preparation) noexcept {
    // Arm each route after the consumer's Activate callback, then satisfy routes whose provider published earlier.
    for (auto& operation : preparation.bindings_) {
        bindings_.push_back({consumer, std::move(operation), false});
        auto& binding = bindings_.back();
        if (const auto* provider = find_provider(binding.operation.provider_patch, false)) {
            try_connect(binding, *provider);
        }
    }
}

void InterfaceRouter::publish_provider(catalog::PatchIndex provider, const planning::PatchInstance& instance,
                                       std::string_view provider_id) noexcept {
    const auto& callbacks = instance.callbacks();
    providers_.push_back({provider, provider_id, callbacks.context, instance.get(), callbacks.query_interface, false});
    const auto& published = providers_.back();
    // Waiting consumers connect before the provider's prepared hooks can begin producing observations.
    for (auto& binding : bindings_) {
        try_connect(binding, published);
    }
}

void InterfaceRouter::mark_active(catalog::PatchIndex provider) noexcept {
    const auto found = std::ranges::find_if(providers_, [&](const ProviderRecord& record) {
        return record.patch == provider;
    });
    if (found != providers_.end()) {
        found->active = true;
    }
}

std::size_t InterfaceRouter::provider_count() const noexcept {
    return providers_.size();
}

std::size_t InterfaceRouter::binding_count() const noexcept {
    return bindings_.size();
}

std::uint64_t InterfaceRouter::noncanonical_result_count() const noexcept {
    return noncanonical_results_;
}

} // namespace fc::runtime
