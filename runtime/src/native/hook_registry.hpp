#pragma once

#include "plan.hpp"

#include <memory>
#include <span>

namespace fc::runtime {

struct HookSite;
struct Participants;

// Retains disabled new sites and replacement participant lists until the plan installs them.
// Borrows existing sites and plan declarations, which must remain alive until installation or disposal.
class HookPreparation {
  public:
    HookPreparation();
    ~HookPreparation();
    HookPreparation(HookPreparation &&) noexcept;
    HookPreparation &operator=(HookPreparation &&) noexcept;

  private:
    struct Item {
        // Points to an existing registry site or to the new site held by owned.
        HookSite *site{};
        std::unique_ptr<HookSite> owned;
        // Includes prior participants and this plan's declarations; installation publishes it atomically.
        std::unique_ptr<Participants> list;
    };

    std::vector<Item> items_;
    friend class HookRegistry;
};

// Owns physical detours and every participant list that a native invocation can capture.
class HookRegistry {
  public:
    HookRegistry();
    // Releases site storage and backend detours; installed entries and handles must be unreachable and all invocations finished.
    ~HookRegistry();

    // Replaces overlapping detour bytes with saved originals; fails if live bytes cannot be read or differ from the installed bytes.
    std::expected<void, Error> original_view(std::uintptr_t, std::vector<std::byte> &) const;
    // Copies saved bytes for a site at this exact address, or returns an empty vector if none exists.
    std::vector<std::byte> original_bytes(std::uintptr_t) const;

    // Builds disabled detours, original-call handles and ordered state layouts for a validated compatible plan.
    // Installed plans must remain at stable addresses while the registry borrows their declarations.
    std::expected<HookPreparation, Error> prepare(SubmittedPlan &);
    // Enables sites and publishes lists prepared for this plan; unsafe failure requires retaining its resources.
    std::expected<void, patching::NativeWriteFailure> install(HookPreparation &, SubmittedPlan &);

  private:
    std::vector<std::unique_ptr<HookSite>> sites_;
};

} // namespace fc::runtime
