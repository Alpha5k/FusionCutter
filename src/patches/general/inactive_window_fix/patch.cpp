#include <FusionCutter/categories.hpp>

#include <FusionCutter/patch.hpp>

#include <cstdint>
#include <span>
#include <utility>

namespace fusioncutter::patches::inactive_window_fix {
namespace {

struct TargetData {
    std::uint32_t sleep_rva;
    std::span<const std::byte> prefix;
    std::span<const std::byte> suffix;
};

constexpr auto kRetailPrefix = byte_array<0xC0, 0x0F, 0x95, 0xC0, 0x84, 0xC0, 0x75, 0xCA>();
constexpr auto kSteamSuffix = byte_array<0xFF, 0xD7, 0xE8, 0xF4, 0x33, 0xF2, 0xFF, 0x84, 0xC0, 0x75, 0xD3>();
constexpr auto kGogSuffix = byte_array<0xFF, 0xD7, 0xE8, 0xE4, 0x30, 0xF2, 0xFF, 0x84, 0xC0, 0x75, 0xD3>();

constexpr auto kModToolsPrefix = byte_array<0xC0, 0x0F, 0x95, 0xC0, 0x84, 0xC0, 0x75, 0xD8>();
constexpr auto kModToolsSuffix = byte_array<0xFF, 0xD5, 0xE8, 0xAA, 0xFE, 0xCC, 0xFF, 0x84, 0xC0, 0x75, 0x0E>();

constexpr auto kPushImmediatePattern = byte_array<0x6A, 0x00>();
constexpr auto kPushImmediateMask = byte_array<0xFF, 0x00>();
constexpr auto kSleepZero = byte_array<0x6A, 0x00>();

constexpr TargetData kSteamTarget{0x00217A93, kRetailPrefix, kSteamSuffix};
constexpr TargetData kGogTarget{0x00218B03, kRetailPrefix, kGogSuffix};
constexpr TargetData kModToolsTarget{0x003383EE, kModToolsPrefix, kModToolsSuffix};

[[nodiscard]] constexpr const TargetData& target_data(TargetLayout layout) noexcept {
    switch (layout) {
    case TargetLayout::SteamRetail:
        return kSteamTarget;
    case TargetLayout::GOGRetail:
        return kGogTarget;
    case TargetLayout::ModTools:
        return kModToolsTarget;
    case TargetLayout::Aspyr:
        std::unreachable();
    }
    std::unreachable();
}

// Changes the inactive-window Sleep(10) call to Sleep(0) without altering the surrounding focus loop.
class InactiveWindowFix final : public Patch {
  public:
    explicit InactiveWindowFix(const TargetContext& target) noexcept : target_(target_data(target.layout)) {}

    void build_plan(PatchPlan& plan) override {
        plan.require_bytes("Verify inactive-window branch", target_.sleep_rva - target_.prefix.size(),
                           BytePattern::exact(target_.prefix));
        plan.require_bytes("Verify inactive-window polling continuation",
                           target_.sleep_rva + kPushImmediatePattern.size(), BytePattern::exact(target_.suffix));

        // The surrounding branch identifies the call, so compatible executables may carry a different delay value.
        plan.checked_write("Remove inactive-window delay", target_.sleep_rva,
                           BytePattern::masked(kPushImmediatePattern, kPushImmediateMask), kSleepZero);
    }

  private:
    const TargetData& target_;
};

const PatchVariants kVariants{
    make_patch_variant<InactiveWindowFix, TargetLayout::SteamRetail>(HostRole::Client, TargetImage::Game),
    make_patch_variant<InactiveWindowFix, TargetLayout::SteamRetail>(HostRole::Server, TargetImage::Game),
    make_patch_variant<InactiveWindowFix, TargetLayout::GOGRetail>(HostRole::Client, TargetImage::Game),
    make_patch_variant<InactiveWindowFix, TargetLayout::GOGRetail>(HostRole::Server, TargetImage::Game),
    make_patch_variant<InactiveWindowFix, TargetLayout::ModTools>(HostRole::Client, TargetImage::Game),
};

} // namespace

PatchDefinition definition() {
    return {
        .name = "Inactive Window Fix",
        .enabled = true,
        .configurable = true,
        .category = categories::GeneralFixes,
        .description = "Prevent the game from throttling updates while the window is unfocused.",
        .variants = kVariants,
    };
}

} // namespace fusioncutter::patches::inactive_window_fix
