#include <FusionCutter/SDK.hpp>

namespace sdk_fixture {

// The external fixture uses the full handler adapter so integration crosses the generated query/registration bridge.
class Handler {
  public:
    Handler() = default;
    ~Handler() noexcept = default;
};

fc::Plugin build_plugin() {
    return fc::plugin({
        .id = "SdkFixture",
        .patches =
            {
                fc::patch<Handler>({
                    .id = "Fixture",
                    .name = "SDK fixture",
                    .supports =
                        {
                            fc::support({
                                .layouts = {fc::TargetLayout::GameSpyRetail},
                                .roles = fc::HostRole::Client,
                                .image = fc::TargetImage::Game,
                            }),
                        },
                }),
            },
    });
}

} // namespace sdk_fixture
