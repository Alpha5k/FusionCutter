#include "profiles.hpp"

#include <array>

#if defined(_M_IX86)
#include "gog_retail.hpp"
#include "mod_tools.hpp"
#include "steam_retail.hpp"
#elif defined(_M_X64)
#include "aspyr.hpp"
#else
#error Unsupported Fusion Cutter target-catalog architecture
#endif

namespace fusioncutter::targets::layouts {
namespace {

#if defined(_M_IX86)
constexpr std::array kKnownProfiles = {
    steam_retail::kGame,         gog_retail::kGame, gog_retail::kGalaxyPeer2017,
    gog_retail::kGalaxyPeer2018, mod_tools::kGame,  mod_tools::kNoDvdNamedGame,
};
#elif defined(_M_X64)
constexpr std::array kKnownProfiles = {
    aspyr::kBootstrap,
    aspyr::kGame,
    aspyr::kOriginalNamedGame,
};
#endif

} // namespace

std::span<const ImageProfile> known_image_profiles() noexcept {
    return kKnownProfiles;
}

} // namespace fusioncutter::targets::layouts
