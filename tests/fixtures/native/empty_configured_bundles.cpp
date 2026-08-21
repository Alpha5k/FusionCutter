#include "configured_bundles.hpp"

namespace fc::catalog {

// The external slice framework deliberately owns no configured plugin factories.
std::span<const RegistrationBridge> configured_bundle_bridges() noexcept {
    return {};
}

} // namespace fc::catalog
