#include <FusionCutter/patch.hpp>

namespace alpha {

#if FC_TEST_HAS_X86_CLIENT_SOURCE
int conditional_source_marker();
#endif

namespace {

class FixturePatch final : public fusioncutter::Patch {
  public:
    explicit FixturePatch(const fusioncutter::TargetContext&) {}
    void build_plan(fusioncutter::PatchPlan&) override {}
};

const fusioncutter::PatchVariants kVariants{
    fusioncutter::make_patch_variant<FixturePatch, fusioncutter::TargetLayout::SteamRetail,
                                     fusioncutter::HostRole::Client>(fusioncutter::TargetImage::Game),
    fusioncutter::make_patch_variant<FixturePatch, fusioncutter::TargetLayout::SteamRetail,
                                     fusioncutter::HostRole::Server>(fusioncutter::TargetImage::Game),
    fusioncutter::make_patch_variant<FixturePatch, fusioncutter::TargetLayout::Aspyr, fusioncutter::HostRole::Client>(
        fusioncutter::TargetImage::Game),
    fusioncutter::make_patch_variant<FixturePatch, fusioncutter::TargetLayout::Aspyr, fusioncutter::HostRole::Server>(
        fusioncutter::TargetImage::Game),
};

} // namespace

fusioncutter::PatchDefinition definition() {
#if FC_TEST_HAS_X86_CLIENT_SOURCE
    static_cast<void>(conditional_source_marker());
#endif
    return {
        .name = "Alpha",
        .enabled = true,
        .configurable = true,
        .category = {"Testing", 100},
        .description = {},
        .settings = {},
        .depends_on = {},
        .includes = {},
        .variants = kVariants,
    };
}

} // namespace alpha
