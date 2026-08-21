#pragma once

#include "patch_transaction.hpp"

#include "../catalog/catalog_types.hpp"
#include "../planning/planning_types.hpp"
#include "../targets/recognition.hpp"

#include <expected>
#include <memory>
#include <span>
#include <vector>

namespace fc::patching {

class HookRegistry;

// A borrowed diagnostic projection identifies executable storage created for one physical shared hook site.
struct HookResourceView {
    std::uintptr_t entry_address{};
    std::size_t entry_size{};
    std::uintptr_t trampoline_address{};
};

using HookResourceVisitor = void (*)(void* context, const HookResourceView& resource) noexcept;

// Owns unpublished snapshots, bindings, and any absent physical sites prepared by one patch attempt.
class HookPreparation final {
  public:
    HookPreparation() noexcept;
    HookPreparation(const HookPreparation&) = delete;
    HookPreparation& operator=(const HookPreparation&) = delete;
    HookPreparation(HookPreparation&&) noexcept;
    HookPreparation& operator=(HookPreparation&&) noexcept;
    ~HookPreparation();

    // Original handles must become unbound before an unexposed plugin instance is destroyed.
    void clear_original_bindings() noexcept;
    // Retained exposed failures copy resource facts before this attempt owner is pinned for process lifetime.
    void visit_resources(void* context, HookResourceVisitor visitor) const noexcept;

  private:
    struct Impl;
    explicit HookPreparation(std::unique_ptr<Impl> implementation) noexcept;

    std::unique_ptr<Impl> implementation_;

    friend class HookRegistry;
};

// Owns one process-lifetime physical hook and atomic snapshot stream for each installed native site.
class HookRegistry final {
  public:
    HookRegistry();
    HookRegistry(const HookRegistry&) = delete;
    HookRegistry& operator=(const HookRegistry&) = delete;
    HookRegistry(HookRegistry&&) = delete;
    HookRegistry& operator=(HookRegistry&&) = delete;
    ~HookRegistry();

    // Capacity is reserved before the first Commit phase so publication is non-allocating and non-failing.
    void reserve(std::size_t maximum_sites);

    [[nodiscard]] std::span<const planning::InstalledHookSite> installed_sites() const noexcept;

    // Prepares closed absent sites and retains the contribution set without exposing callbacks or Original handles.
    [[nodiscard]] std::expected<HookPreparation, planning::FailureReason>
    prepare_patch(const targets::RecognizedTarget& target, const catalog::Catalog& catalog,
                  const planning::PatchWorkRecord& patch, PatchTransaction& transaction,
                  std::vector<planning::MemoryClaim>& claims);

    // Binds each callable original after native state visible to the Prepare callback passes revalidation.
    [[nodiscard]] std::expected<void, planning::FailureReason> bind_originals(HookPreparation& preparation);

    // Allocates immutable participant views after the Prepare callback and revalidation, before the Commit phase.
    [[nodiscard]] std::expected<void, planning::FailureReason> finalize_patch(HookPreparation& preparation,
                                                                              const catalog::Catalog& catalog);

    // Transfers prepared ownership and publishes callbacks with release semantics only after the plugin has activated.
    void publish(HookPreparation preparation) noexcept;
    // Successful crash publication visits only physical resources first created by the named installed patch.
    void visit_patch_resources(catalog::PatchIndex patch, void* context, HookResourceVisitor visitor) const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> implementation_;
};

} // namespace fc::patching
