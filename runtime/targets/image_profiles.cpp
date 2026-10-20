#include "images/image.hpp"

#include <array>

namespace fc::runtime {

namespace {

// Battlefront 1

constexpr std::array kBF1SteamFilenames{std::string_view{"Battlefront.exe"}};
constexpr std::array kBF1SteamSections{SectionProfile{".text", 0x1000, 0x40ac05}, SectionProfile{".rdata", 0x40c000, 0x6acad},
                                       SectionProfile{".data", 0x477000, 0x1d44b60}};

constexpr std::array kBF1SPTestFilenames{std::string_view{"SPTest.exe"}};
constexpr std::array kBF1SPTest2004Sections{SectionProfile{".text", 0x1000, 0x1ff6b2}, SectionProfile{".rdata", 0x201000, 0x4b5b6},
                                            SectionProfile{".data", 0x24d000, 0x185d578}};
constexpr std::array kBF1SPTest2005Sections{SectionProfile{".text", 0x1000, 0x2002a5}, SectionProfile{".rdata", 0x202000, 0x4c486},
                                            SectionProfile{".data", 0x24f000, 0x185fd18}};

constexpr std::array kBF1DVDFilenames{std::string_view{"Battlefront.exe"}, std::string_view{"Battlefront SWBFSpy.exe"}};
constexpr std::array kBF1DVDSections{SectionProfile{".text", 0x1000, 0x29b000}, SectionProfile{".rdata", 0x29c000, 0x4dad6},
                                     SectionProfile{".data", 0x2ea000, 0x1854d58}};

constexpr std::array kBF1ClassicFilenames{std::string_view{"Battlefront1.dll"}, std::string_view{"Battlefront1.original.dll"}};
constexpr std::array kBF1ClassicSections{SectionProfile{".text", 0x1000, 0x3b4dac}, SectionProfile{".rdata", 0x3b6000, 0xc6610},
                                         SectionProfile{".data", 0x47d000, 0x194e660}};

// Battlefront 2

constexpr std::array kBF2SteamFilenames{std::string_view{"BattlefrontII.exe"}};
constexpr std::array kBF2SteamSections{SectionProfile{".text", 0x1000, 0x36950b}, SectionProfile{".rdata", 0x36b000, 0x726a8},
                                       SectionProfile{".data", 0x3de000, 0x17ce3c8}};

constexpr std::array kBF2GOGFilenames{std::string_view{"BattlefrontII.exe"}};
constexpr std::array kBF2GOGSections{SectionProfile{".text", 0x1000, 0x36a7bb}, SectionProfile{".rdata", 0x36c000, 0x72e46},
                                     SectionProfile{".data", 0x3df000, 0x17ce878}};

constexpr std::array kBF2DVDFilenames{std::string_view{"BattlefrontII.exe"}, std::string_view{"BattlefrontII_DVD.exe"}};
constexpr std::array kBF2DVDSections{SectionProfile{".text", 0x1000, 0x38b0ab}, SectionProfile{".rdata", 0x38d000, 0x50648},
                                     SectionProfile{".data", 0x3de000, 0x1811cd8}};

constexpr std::array kBF2ModToolsFilenames{std::string_view{"BF2_modtools.exe"}, std::string_view{"BF2_modtools_NoDVD.exe"}};
constexpr std::array kBF2ModToolsSections{SectionProfile{".text", 0x1000, 0x6288ac}, SectionProfile{".rdata", 0x62a000, 0x98f47},
                                          SectionProfile{".data", 0x6c3000, 0x188870c}};

constexpr std::array kClassicBootstrapFilenames{std::string_view{"Battlefront.exe"}};
constexpr std::array kClassicBootstrapSections{SectionProfile{".text", 0x1000, 0xba5fc}, SectionProfile{".rdata", 0xbc000, 0x316b0},
                                               SectionProfile{".data", 0xee000, 0x13860}};

constexpr std::array kBF2ClassicFilenames{std::string_view{"Battlefront2.dll"}, std::string_view{"Battlefront2.original.dll"}};
constexpr std::array kBF2ClassicSections{SectionProfile{".text", 0x1000, 0x4ff19c}, SectionProfile{".rdata", 0x501000, 0x1375c0},
                                         SectionProfile{".data", 0x639000, 0x1dbc550}};

// GOG Galaxy Helper DLLs

constexpr std::array kGalaxyPeerFilenames{std::string_view{"GalaxyPeer.dll"}};
constexpr std::array kGalaxyPeer2017Sections{SectionProfile{".text", 0x1000, 0x84be16}, SectionProfile{".rdata", 0x84d000, 0x17afa0},
                                             SectionProfile{".data", 0x9c8000, 0x88320}};

constexpr std::array kGalaxyPeer2018Sections{SectionProfile{".text", 0x1000, 0x883e65}, SectionProfile{".rdata", 0x885000, 0x18da68},
                                             SectionProfile{".data", 0xa13000, 0x76550}};

constexpr std::array kGalaxyFilenames{std::string_view{"Galaxy.dll"}};
constexpr std::array kGalaxySections{SectionProfile{".text", 0x1000, 0x3687de}, SectionProfile{".rdata", 0x36a000, 0xa13a5},
                                     SectionProfile{".data", 0x40c000, 0x1a0a0}};

// Each runtime build recognizes images for its own architecture using image sizes and PE sections.
// Extra sections are compatible when the required sections still match their names, RVAs and VirtualSizes.
#if defined(_WIN64)
constexpr std::array kProfiles{
    ImageProfile{"Classic.Bootstrap.66702CD7", kClassicBootstrapFilenames, 0x14e000, kClassicBootstrapSections},
    ImageProfile{"BF1.Classic.66702CCB", kBF1ClassicFilenames, 0x1e1f000, kBF1ClassicSections},
    ImageProfile{"BF2.Classic.66702CD2", kBF2ClassicFilenames, 0x245f000, kBF2ClassicSections},
};
#else
constexpr std::array kProfiles{
    ImageProfile{"BF1.Steam.5F243336", kBF1SteamFilenames, 0x2224000, kBF1SteamSections},
    ImageProfile{"BF1.SPTest.41C75543", kBF1SPTestFilenames, 0x1aae000, kBF1SPTest2004Sections},
    ImageProfile{"BF1.SPTest.41EC6058", kBF1SPTestFilenames, 0x1ab2000, kBF1SPTest2005Sections},
    ImageProfile{"BF1.DVD.41E48CFC", kBF1DVDFilenames, 0x1b414f0, kBF1DVDSections},
    ImageProfile{"BF2.Steam.59EDE353", kBF2SteamFilenames, 0x1be4000, kBF2SteamSections},
    ImageProfile{"BF2.GOG.59EDF52B", kBF2GOGFilenames, 0x1be5000, kBF2GOGSections},
    ImageProfile{"BF2.DVD.44961FB6", kBF2DVDFilenames, 0x1bf3000, kBF2DVDSections},
    ImageProfile{"BF2.ModTools.43EBD102", kBF2ModToolsFilenames, 0x1f51000, kBF2ModToolsSections},
    ImageProfile{"GalaxyPeer.59E6304A", kGalaxyPeerFilenames, 0xac7000, kGalaxyPeer2017Sections},
    ImageProfile{"GalaxyPeer.5BBE22A6", kGalaxyPeerFilenames, 0xaf3000, kGalaxyPeer2018Sections},
    ImageProfile{"Galaxy.59E63011", kGalaxyFilenames, 0x456000, kGalaxySections},
};

#endif

} // namespace

std::span<const ImageProfile> known_image_profiles() noexcept {
    return kProfiles;
}

} // namespace fc::runtime
