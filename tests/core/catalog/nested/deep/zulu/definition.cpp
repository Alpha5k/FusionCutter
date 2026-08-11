#include <FusionCutter/patch.hpp>

namespace zulu {
namespace {

class FixturePatch final : public fusioncutter::Patch {
  public:
    explicit FixturePatch(const fusioncutter::TargetContext&) {}
    void build_plan(fusioncutter::PatchPlan&) override {}
};

const fusioncutter::PatchVariants kVariants{
    fusioncutter::make_patch_variant<FixturePatch, fusioncutter::TargetLayout::SteamRetail>(
        fusioncutter::HostRole::Client, fusioncutter::TargetImage::Game),
    fusioncutter::make_patch_variant<FixturePatch, fusioncutter::TargetLayout::SteamRetail>(
        fusioncutter::HostRole::Server, fusioncutter::TargetImage::Game),
    fusioncutter::make_patch_variant<FixturePatch, fusioncutter::TargetLayout::Aspyr>(fusioncutter::HostRole::Client,
                                                                                      fusioncutter::TargetImage::Game),
    fusioncutter::make_patch_variant<FixturePatch, fusioncutter::TargetLayout::Aspyr>(fusioncutter::HostRole::Server,
                                                                                      fusioncutter::TargetImage::Game),
};

} // namespace

fusioncutter::PatchDefinition definition() {
    return {
        .name = "Zulu",
        .enabled = false,
        .configurable = false,
        .category = {"Testing", 100},
        .description = {},
        .settings = {},
        .depends_on = {},
        .includes = {},
        .variants = kVariants,
    };
}

} // namespace zulu
