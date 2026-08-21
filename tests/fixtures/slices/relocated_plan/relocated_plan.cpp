#include "relocated_plan.hpp"

#include <cstdint>
#include <utility>

namespace fc::fixtures::relocated_plan {
namespace {

using TargetFunction = void(FC_CALL*)() noexcept;

// Settings affect symbolic storage and mutable image access without creating a second planning path.
struct StorageSettings {
    std::uint32_t allocation_count = 4;
    bool use_mutable_location = true;
};

// The independent compact patch consumes typed configuration without acquiring persistent handler state.
struct IndependentSettings {
    std::uint32_t replacement = 0xCCDD'0011;
};

// Describes symbolic storage and representative native requirements from one selected semantic layout.
class StorageHandler final {
  public:
    using Settings = StorageSettings;

    StorageHandler(const fc::CreateContext& context, const Settings& settings) noexcept
        : target_(context.target()), settings_(settings) {}

    // Submits the related allocation, address, branch, and access forms as one independently valid patch plan.
    void plan(fc::Plan& plan) {
        const auto layout = layout_for(target_);

        // Symbolic storage stays unresolved while absolute and relative interior addresses enter the common plan.
        const auto allocation = plan.allocate_data<std::uint32_t>(settings_.allocation_count, "Relocated values");
        plan.write_at(layout.pointer_slot, allocation.element(1));
        plan.write_at(layout.relative_slot, fc::rel32(allocation.element(2)));

        // The branch and requirements preserve distinct code, vtable, and mutable data access policies.
        const fc::FunctionLocation<TargetFunction> function{.rva = layout.function, .name = "RelocatedTarget"};
        plan.write_at(layout.call_site, fc::call_to(function));
        (void)plan.require(fc::CodeLocation{.rva = layout.code, .name = "RelocatedCode"}, 8);
        (void)plan.require(fc::VtableLocation{.rva = layout.vtable, .name = "RelocatedVtable"}, sizeof(void*) * 2);
        if (settings_.use_mutable_location) {
            (void)plan.require_mutable(
                fc::DataLocation<std::uint32_t>{.rva = layout.mutable_data, .name = "RelocatedMutableData"});
        }
        plan.logger().debug("Planned {} relocated values for profile {} with mutable access {}",
                            settings_.allocation_count, target_.image_profile, settings_.use_mutable_location);
    }

  private:
    fc::TargetInfo target_;
    Settings settings_;
};

// Keeps configuration of the allocation and installed mutable access on the ordinary typed settings path.
[[nodiscard]] fc::SettingsSchema<StorageSettings> storage_settings() {
    return fc::settings<StorageSettings>(
        fc::value("AllocationCount", &StorageSettings::allocation_count, std::uint32_t{4})
            .range(std::uint32_t{3}, std::uint32_t{16})
            .description("Number of values owned by the framework and reserved for the relocated plan"),
        fc::value("UseMutableLocation", &StorageSettings::use_mutable_location, true)
            .description("Reserve the reviewed mutable image location for access while installed"));
}

// A separate typed schema keeps configuration for the compact patch on the ordinary conversion and validation path.
[[nodiscard]] fc::SettingsSchema<IndependentSettings> independent_settings() {
    return fc::settings<IndependentSettings>(
        fc::value("Replacement", &IndependentSettings::replacement, std::uint32_t{0xCCDD'0011})
            .description("Value written by the independent compact patch"));
}

// Both architecture-specific target rows use the same patch composition and planning behavior.
[[nodiscard]] fc::Support support() {
    return fc::support({.layouts = {fc::TargetLayout::SteamRetail, fc::TargetLayout::ClassicCollection},
                        .roles = fc::HostRole::Client,
                        .image = fc::TargetImage::Game});
}

// Captures the two reviewed target rows by value so compact patches own immutable facts without persistent state.
struct ConflictLayout {
    fc::Rva steam;
    fc::Rva classic_collection;

    [[nodiscard]] constexpr fc::Rva for_target(fc::TargetLayout target) const noexcept {
        return target == fc::TargetLayout::SteamRetail ? steam : classic_collection;
    }
};

// Produces one compact write whose overlap can be varied without introducing another fixture plugin.
[[nodiscard]] fc::Patch conflict_patch(std::string id, std::uint32_t byte_offset, std::uint32_t replacement) {
    const ConflictLayout layout{layout_for({.layout = fc::TargetLayout::SteamRetail}).conflict,
                                layout_for({.layout = fc::TargetLayout::ClassicCollection}).conflict};
    return fc::plan_patch(
        {.id = std::move(id), .name = "Relocated conflict participant", .category = "Memory", .supports = {support()}},
        [layout, byte_offset, replacement](fc::Plan& plan) {
            const auto location = layout.for_target(plan.target().layout);
            plan.write_at({location.value + byte_offset}, replacement);
            plan.logger().debug("Planned conflict participant at byte offset {}", byte_offset);
        });
}

} // namespace

fc::Plugin build_plugin() {
    return fc::plugin({
        .id = std::string{kPluginId},
        .version = "1.0.0",
        .categories = {{.id = "Memory", .order = 10}, {.id = "Isolation", .order = 20}},
        .groups =
            {
                {.id = std::string{kGroupId},
                 .members = {std::string{kStoragePatchId}, std::string{kIndependentPatchId}},
                 .configurable = true,
                 .enabled = true,
                 .category = "Memory",
                 .description = "Relocated storage and isolation validation fixtures"},
            },
        .patches =
            {
                fc::patch<StorageHandler>({.id = std::string{kStoragePatchId},
                                           .name = "Relocated storage plan",
                                           .enabled = true,
                                           .configurable = false,
                                           .category = "Memory",
                                           .settings = storage_settings(),
                                           .supports = {support()}}),
                conflict_patch(std::string{kConflictAPatchId}, 0, 0xAAAAAAAA),
                conflict_patch(std::string{kConflictBPatchId}, 2, 0xBBBBBBBB),
                fc::plan_patch({.id = std::string{kIndependentPatchId},
                                .name = "Relocated independent write",
                                .enabled = true,
                                .configurable = false,
                                .category = "Isolation",
                                .settings = independent_settings(),
                                .supports = {support()}},
                               [](fc::Plan& plan, const IndependentSettings& settings) {
                                   plan.write_at(layout_for(plan.target()).independent, settings.replacement);
                                   plan.logger().debug("Planned the independent relocated write with value {}",
                                                       settings.replacement);
                               }),
            },
    });
}

} // namespace fc::fixtures::relocated_plan
