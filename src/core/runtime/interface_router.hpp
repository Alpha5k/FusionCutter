#pragma once

#include "../core_logger.hpp"
#include "../catalog/catalog_types.hpp"
#include "../planning/planning_types.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>
#include <vector>

namespace fc::runtime {

// Owns the copied routes for optional bindings prepared by one patch before native state can be exposed.
class InterfacePreparation final {
  public:
    InterfacePreparation() = default;
    InterfacePreparation(const InterfacePreparation&) = delete;
    InterfacePreparation& operator=(const InterfacePreparation&) = delete;
    InterfacePreparation(InterfacePreparation&&) noexcept = default;
    InterfacePreparation& operator=(InterfacePreparation&&) noexcept = default;

  private:
    std::vector<planning::InterfaceBindingOperation> bindings_;

    friend class InterfaceRouter;
};

// Routes interface values with their exact layout once, then connects the runtime provider and consumer directly.
class InterfaceRouter final {
  public:
    // Retains the framework's interface scope so ABI violations and one-time binding decisions remain diagnosable.
    void set_logger(CoreLogger logger) noexcept;

    // Reservation occurs before the Commit phase so activation appends into process-lifetime storage.
    void reserve(std::size_t provider_capacity, std::size_t binding_capacity);

    // Copies binding records from the patch plan while failure can still follow the ordinary path before exposure.
    [[nodiscard]] std::expected<InterfacePreparation, planning::FailureReason>
    prepare(std::span<const planning::OperationRecord> operations) const;

    // Immediate lookup during the Prepare phase sees only providers that completed publication.
    [[nodiscard]] FC_Bool find_active(std::string_view provider_patch, std::string_view id, std::uint32_t size,
                                      void* output) const noexcept;

    // Activation first arms the consumer's routes, then exposes the provider to all already-armed consumers.
    void connect_consumer(catalog::PatchIndex consumer, InterfacePreparation preparation) noexcept;
    void publish_provider(catalog::PatchIndex provider, const planning::PatchInstance& instance,
                          std::string_view provider_id) noexcept;
    void mark_active(catalog::PatchIndex provider) noexcept;

    [[nodiscard]] std::size_t provider_count() const noexcept;
    [[nodiscard]] std::size_t binding_count() const noexcept;
    [[nodiscard]] std::uint64_t noncanonical_result_count() const noexcept;

  private:
    // Published provider records borrow process-lifetime IDs and admitted callback state from the plugin catalog.
    struct ProviderRecord {
        catalog::PatchIndex patch;
        std::string_view id;
        void* callback_context{};
        FC_PatchHandle handle{};
        FC_PatchQueryInterfaceFn query{};
        bool active{};
    };

    // A binding retains the thunk copied from the Plan callback and records its one permitted successful delivery.
    struct BindingRecord {
        catalog::PatchIndex consumer;
        planning::InterfaceBindingOperation operation;
        bool connected{};
    };

    // Internal lookup centralizes case folding, scratch storage matching the layout, delivery, and the native boundary.
    [[nodiscard]] const ProviderRecord* find_provider(std::string_view id, bool require_active) const noexcept;
    [[nodiscard]] bool query(const ProviderRecord& provider, std::string_view id, std::uint32_t size,
                             void* output) const noexcept;
    void try_connect(BindingRecord& binding, const ProviderRecord& provider) noexcept;

    std::vector<ProviderRecord> providers_;
    std::vector<BindingRecord> bindings_;
    CoreLogger logger_;
    mutable std::uint64_t noncanonical_results_{};
};

} // namespace fc::runtime
