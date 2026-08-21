#include "../core/catalog/catalog_builder.hpp"

#include <FusionCutter/SDK.hpp>

namespace {

// The built-in Core plugin begins empty and grows through the same immutable SDK composition surface as other plugins.
fc::Plugin build_core_plugin() {
    return fc::plugin({.id = "Core"});
}

} // namespace

namespace fc::catalog {

RegistrationBridge core_registration_bridge() noexcept {
    // The built-in Core plugin crosses the SDK adapter so its metadata, callbacks, and cleanup follow normal admission.
    return {fc::detail::bundled_registration<&build_core_plugin>(),
            &fc::detail::release_registration<&build_core_plugin>};
}

} // namespace fc::catalog
