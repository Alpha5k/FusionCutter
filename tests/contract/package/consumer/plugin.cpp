#include <FusionCutter/SDK.hpp>

namespace package_consumer {

// Compiling this factory only through the installed package proves exported headers and bridge helpers are complete.
fc::Plugin build_plugin() {
    return fc::plugin({
        .id = "PackageConsumer",
        .patches =
            {
                fc::plan_patch(
                    {
                        .id = "Write",
                        .name = "Package write",
                        .supports =
                            {
                                fc::support({
                                    .layouts = {fc::TargetLayout::GameSpyRetail},
                                    .roles = fc::HostRole::Client,
                                    .image = fc::TargetImage::Game,
                                }),
                            },
                    },
                    [](fc::Plan& plan) {
                        plan.write_at({0x1000}, std::uint32_t{1});
                    }),
            },
    });
}

} // namespace package_consumer
