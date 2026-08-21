#include <FusionCutter/SDK.hpp>

#include <stdexcept>

extern "C" __declspec(dllimport) int fc_probe_dependency_value() noexcept;

namespace dependent_probe {
namespace {

// A no-op handler keeps this fixture focused on dependency loading rather than patch lifecycle behavior.
class Handler {
  public:
    Handler() = default;
    ~Handler() noexcept = default;
};

} // namespace

fc::Plugin build_plugin() {
    // Touch the imported symbol during registration so admission proves the approved private dependency was loaded.
    if (fc_probe_dependency_value() != 42) {
        throw std::runtime_error{"The dependent DLL returned an unexpected value"};
    }
    return fc::plugin({
        .id = "DependentProbe",
        .patches =
            {
                fc::patch<Handler>({
                    .id = "DependentPatch",
                    .name = "Dependent DLL probe",
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

} // namespace dependent_probe
