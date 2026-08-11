#include <FusionCutter/patch.hpp>

namespace client_only {
namespace {

class FixturePatch final : public fusioncutter::Patch {
  public:
    explicit FixturePatch(const fusioncutter::TargetContext&) {}
    void build_plan(fusioncutter::PatchPlan&) override {}
};

const fusioncutter::PatchVariants kVariants{
    fusioncutter::make_patch_variant<FixturePatch, fusioncutter::TargetLayout::SteamRetail>(
        fusioncutter::HostRole::Client, fusioncutter::TargetImage::Game),
};

} // namespace

fusioncutter::PatchDefinition definition() {
    return {
        .name = "Client Only",
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

} // namespace client_only
