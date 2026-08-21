#include "target_profiles.hpp"

#include "../catalog/definition_copy.hpp"

namespace fc::targets {
namespace {

// Basename arrays preserve every reviewed launcher or module alias accepted for one physical image.
constexpr std::array kSteamNames{std::string_view{"BattlefrontII.exe"}};
constexpr std::array kGogNames{std::string_view{"BattlefrontII.exe"}};
constexpr std::array kGalaxyNames{std::string_view{"GalaxyPeer.dll"}};
constexpr std::array kModToolsNames{std::string_view{"BF2_modtools.exe"}, std::string_view{"BF2_modtools_NoDVD.exe"}};
constexpr std::array kClassicBootstrapNames{std::string_view{"Battlefront.exe"}};
constexpr std::array kClassicGameNames{std::string_view{"Battlefront2.dll"},
                                       std::string_view{"Battlefront2.original.dll"}};

// Section fingerprints use mapped RVAs and virtual sizes because recognition inspects images in their loaded layout.
constexpr std::array kSteamSections{SectionProfile{".text", 0x1000, 0x36950b},
                                    SectionProfile{".rdata", 0x36b000, 0x726a8},
                                    SectionProfile{".data", 0x3de000, 0x17ce3c8}};
constexpr std::array kGogSections{SectionProfile{".text", 0x1000, 0x36a7bb},
                                  SectionProfile{".rdata", 0x36c000, 0x72e46},
                                  SectionProfile{".data", 0x3df000, 0x17ce878}};
constexpr std::array kGalaxy2017Sections{SectionProfile{".text", 0x1000, 0x84be16},
                                         SectionProfile{".rdata", 0x84d000, 0x17afa0},
                                         SectionProfile{".data", 0x9c8000, 0x88320}};
constexpr std::array kGalaxy2018Sections{SectionProfile{".text", 0x1000, 0x883e65},
                                         SectionProfile{".rdata", 0x885000, 0x18da68},
                                         SectionProfile{".data", 0xa13000, 0x76550}};
constexpr std::array kModToolsSections{SectionProfile{".text", 0x1000, 0x6288ac},
                                       SectionProfile{".rdata", 0x62a000, 0x98f47},
                                       SectionProfile{".data", 0x6c3000, 0x188870c}};
constexpr std::array kClassicBootstrapSections{SectionProfile{".text", 0x1000, 0xba5fc},
                                               SectionProfile{".rdata", 0xbc000, 0x316b0},
                                               SectionProfile{".data", 0xee000, 0x13860}};
constexpr std::array kClassicGameSections{SectionProfile{".text", 0x1000, 0x4ff19c},
                                          SectionProfile{".rdata", 0x501000, 0x1375c0},
                                          SectionProfile{".data", 0x639000, 0x1dbc550}};

#if defined(FC_SLICE_TEST_PROFILE)
// Purpose-built slice hosts have fixed layouts across configurations and are admitted only by test framework variants.
constexpr std::array kSliceHostNames{std::string_view{"FusionCutterSliceHost.exe"}};
constexpr std::array kSlicePeerNames{std::string_view{"FusionCutterSlicePeer.dll"}};
constexpr std::array kSliceHostX86Sections{SectionProfile{".fcdata", 0x6000, 0x2040}};
constexpr std::array kSliceHostX64Sections{SectionProfile{".fcdata", 0x7000, 0x2040}};
constexpr std::array kSlicePeerSections{SectionProfile{".fclate", 0x1000, 0xf}, SectionProfile{".fcdata", 0x3000, 0x4}};
constexpr LateImagePolicy kSlicePeerLate{L"FusionCutterSlicePeer.dll", LateMutationPolicy::SuspendedHooksOnly};
#endif

// GalaxyPeer may already execute when the 50 ms pump discovers it. Approved late plans use only SafetyHook hooks,
// whose publication suspends threads and repairs affected instruction pointers.
constexpr LateImagePolicy kGalaxyPeerLate{L"GalaxyPeer.dll", LateMutationPolicy::SuspendedHooksOnly};

// Stable IDs identify the reviewed builds; a missing timestamp means the remaining exact facts form the fingerprint.
// GameSpy is deliberately absent until its exact executable fingerprint has been reviewed.
constexpr std::array kProfiles{
    ImageProfile{"SteamRetail_Game_59EDE353", kSteamNames, FC_LAYOUT_STEAM_RETAIL, FC_IMAGE_GAME, FC_ARCH_X86,
                 std::nullopt, 0x1be4000, std::nullopt, kSteamSections},
    ImageProfile{"GOGRetail_Game_59EDF52B", kGogNames, FC_LAYOUT_GOG_RETAIL, FC_IMAGE_GAME, FC_ARCH_X86, std::nullopt,
                 0x1be5000, std::nullopt, kGogSections},
#if defined(FC_SLICE_TEST_PROFILE)
    // The test-only peer profile precedes shipping GalaxyPeer profiles so its distinct basename is probed first.
    ImageProfile{"SlicePeer_GalaxyPeer_X86", kSlicePeerNames, FC_LAYOUT_GOG_RETAIL, FC_IMAGE_GALAXY_PEER, FC_ARCH_X86,
                 kSlicePeerLate, 0x5000, std::nullopt, kSlicePeerSections},
#endif
    ImageProfile{"GOGRetail_GalaxyPeer_59E6304A", kGalaxyNames, FC_LAYOUT_GOG_RETAIL, FC_IMAGE_GALAXY_PEER, FC_ARCH_X86,
                 kGalaxyPeerLate, 0xac7000, std::nullopt, kGalaxy2017Sections},
    ImageProfile{"GOGRetail_GalaxyPeer_5BBE22A6", kGalaxyNames, FC_LAYOUT_GOG_RETAIL, FC_IMAGE_GALAXY_PEER, FC_ARCH_X86,
                 kGalaxyPeerLate, 0xaf3000, std::nullopt, kGalaxy2018Sections},
    ImageProfile{"ModTools_Game_43EBD102", kModToolsNames, FC_LAYOUT_MOD_TOOLS, FC_IMAGE_GAME, FC_ARCH_X86,
                 std::nullopt, 0x1f51000, std::nullopt, kModToolsSections},
    ImageProfile{"ClassicCollection_Bootstrap_66702CD7", kClassicBootstrapNames, FC_LAYOUT_CLASSIC_COLLECTION,
                 FC_IMAGE_BOOTSTRAP, FC_ARCH_X64, std::nullopt, 0x14e000, std::nullopt, kClassicBootstrapSections},
    ImageProfile{"ClassicCollection_Game_66702CD2", kClassicGameNames, FC_LAYOUT_CLASSIC_COLLECTION, FC_IMAGE_GAME,
                 FC_ARCH_X64, std::nullopt, 0x245f000, std::nullopt, kClassicGameSections},
#if defined(FC_SLICE_TEST_PROFILE)
    ImageProfile{"SliceHost_Game_X86", kSliceHostNames, FC_LAYOUT_GOG_RETAIL, FC_IMAGE_GAME, FC_ARCH_X86, std::nullopt,
                 0xa000, std::nullopt, kSliceHostX86Sections},
    ImageProfile{"SliceHost_Game_X64", kSliceHostNames, FC_LAYOUT_CLASSIC_COLLECTION, FC_IMAGE_GAME, FC_ARCH_X64,
                 std::nullopt, 0xb000, std::nullopt, kSliceHostX64Sections},
#endif
};

} // namespace

std::span<const ImageProfile> known_image_profiles() noexcept {
    return kProfiles;
}

const ImageProfile* find_image_profile(std::string_view id) noexcept {
    // Profile lookup is case-insensitive but returns the canonical spelling.
    for (const auto& profile : kProfiles) {
        if (catalog::equal_ascii_case_insensitive(profile.id, id)) {
            return &profile;
        }
    }
    return nullptr;
}

bool valid_target_tuple(FC_TargetLayout layout, FC_HostRole role, FC_TargetImage image) noexcept {
    if (layout == FC_LAYOUT_GAMESPY_RETAIL || layout == FC_LAYOUT_STEAM_RETAIL) {
        return (role == FC_HOST_ROLE_CLIENT || role == FC_HOST_ROLE_SERVER) && image == FC_IMAGE_GAME;
    }
    if (layout == FC_LAYOUT_GOG_RETAIL) {
        return (role == FC_HOST_ROLE_CLIENT && image == FC_IMAGE_GAME) ||
               (role == FC_HOST_ROLE_SERVER && (image == FC_IMAGE_GAME || image == FC_IMAGE_GALAXY_PEER));
    }
    if (layout == FC_LAYOUT_MOD_TOOLS) {
        return role == FC_HOST_ROLE_CLIENT && image == FC_IMAGE_GAME;
    }
    if (layout == FC_LAYOUT_CLASSIC_COLLECTION) {
        return (role == FC_HOST_ROLE_CLIENT && (image == FC_IMAGE_BOOTSTRAP || image == FC_IMAGE_GAME)) ||
               (role == FC_HOST_ROLE_SERVER && image == FC_IMAGE_GAME);
    }
    return false;
}

bool valid_target_role(FC_TargetLayout layout, FC_HostRole role) noexcept {
    if (role != FC_HOST_ROLE_CLIENT && role != FC_HOST_ROLE_SERVER) {
        return false;
    }
    switch (layout) {
    case FC_LAYOUT_GAMESPY_RETAIL:
    case FC_LAYOUT_STEAM_RETAIL:
    case FC_LAYOUT_GOG_RETAIL:
    case FC_LAYOUT_CLASSIC_COLLECTION:
        return true;
    case FC_LAYOUT_MOD_TOOLS:
        return role == FC_HOST_ROLE_CLIENT;
    default:
        return false;
    }
}

} // namespace fc::targets
