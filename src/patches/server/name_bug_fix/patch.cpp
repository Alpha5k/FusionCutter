#include "patch.hpp"

#include <FusionCutter/categories.hpp>

namespace fusioncutter::patches::name_bug_fix {
namespace {

constexpr std::uint32_t kShellDisconnectCallRva = 0x001DDFDF;
constexpr auto kShellDisconnectCall = byte_array<0xE8, 0xAC, 0x5A, 0xFD, 0xFF>();
constexpr std::uint32_t kPostLoadDisconnectCallRva = 0x001E42E4;
constexpr auto kPostLoadDisconnectCall = byte_array<0xE8, 0xA7, 0xF7, 0xFC, 0xFF>();
constexpr std::uint32_t kSetNotPlayingRva = 0x001B9440;

// Routes pre-play disconnects through the native membership cleanup transition.
class NameBugFix final : public RuntimePatch {
  public:
    explicit NameBugFix(const TargetContext& target) noexcept
        : set_not_playing_(target.image.function_at_rva<SetNotPlaying>(kSetNotPlayingRva)) {}

    void build_plan(PatchPlan& plan) override {
        plan.redirect_call("Clean shell-stage membership", kShellDisconnectCallRva,
                           BytePattern::exact(kShellDisconnectCall), &NameBugFix::disconnect_hook);
        plan.redirect_call("Clean post-load membership", kPostLoadDisconnectCallRva,
                           BytePattern::exact(kPostLoadDisconnectCall), &NameBugFix::disconnect_hook);
    }

    void enable_runtime() noexcept override {
        instance_.publish(*this);
    }

    void disable_runtime() noexcept override {
        instance_.clear(*this);
    }

  private:
    using SetNotPlaying = void(__cdecl*)(int);

    SetNotPlaying set_not_playing_;
    static PatchInstanceSlot<NameBugFix> instance_;

    // Adapts the caller's ECX player index to SetNotPlaying's stack argument.
    static void __fastcall disconnect_hook(int player, void*) noexcept {
        if (const auto* patch = instance_.read(); patch != nullptr) {
            patch->set_not_playing_(player);
        }
    }
};

PatchInstanceSlot<NameBugFix> NameBugFix::instance_;

const PatchVariants kVariants{
    make_patch_variant<NameBugFix, TargetLayout::GOGRetail, HostRole::Server>(TargetImage::Game),
};

} // namespace

PatchDefinition definition() {
    return {
        .name = "Name Bug Fix",
        .enabled = true,
        .configurable = true,
        .category = categories::Server,
        .description = "Fix name bug where players who crash/leave during warmup aren't removed properly.",
        .variants = kVariants,
    };
}

} // namespace fusioncutter::patches::name_bug_fix
