#include "recognition.hpp"

#include <catch2/catch_test_macros.hpp>

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

template <class Value> void write_at(std::vector<std::byte>& image, std::size_t offset, const Value& value) {
    REQUIRE(offset <= image.size());
    REQUIRE(sizeof(value) <= image.size() - offset);
    std::memcpy(image.data() + offset, &value, sizeof(value));
}

// Synthesizes only the mapped PE facts consumed by recognition, directly from one reviewed profile.
[[nodiscard]] std::vector<std::byte> mapped_image(const fc::targets::ImageProfile& profile) {
    std::vector<std::byte> image(profile.size_of_image);
    // Construct a minimal consistent DOS/COFF header pair at a fixed, in-bounds NT-header offset.
    IMAGE_DOS_HEADER dos{.e_magic = IMAGE_DOS_SIGNATURE, .e_lfanew = 0x80};
    write_at(image, 0, dos);
    constexpr DWORD signature = IMAGE_NT_SIGNATURE;
    write_at(image, 0x80, signature);

    IMAGE_FILE_HEADER file{};
    file.Machine = profile.architecture == FC_ARCH_X86 ? IMAGE_FILE_MACHINE_I386 : IMAGE_FILE_MACHINE_AMD64;
    file.NumberOfSections = static_cast<WORD>(profile.sections.size());
    file.TimeDateStamp = profile.timestamp.value_or(0);
    file.SizeOfOptionalHeader =
        profile.architecture == FC_ARCH_X86 ? sizeof(IMAGE_OPTIONAL_HEADER32) : sizeof(IMAGE_OPTIONAL_HEADER64);
    write_at(image, 0x84, file);
    // Emit the architecture-specific optional header while keeping the semantic image facts identical.
    const auto optional_offset = 0x84 + sizeof(file);
    if (profile.architecture == FC_ARCH_X86) {
        IMAGE_OPTIONAL_HEADER32 optional{};
        optional.Magic = IMAGE_NT_OPTIONAL_HDR32_MAGIC;
        optional.SizeOfImage = profile.size_of_image;
        optional.SizeOfHeaders = 0x400;
        write_at(image, optional_offset, optional);
    } else {
        IMAGE_OPTIONAL_HEADER64 optional{};
        optional.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
        optional.SizeOfImage = profile.size_of_image;
        optional.SizeOfHeaders = 0x400;
        write_at(image, optional_offset, optional);
    }
    // Section permissions mirror the production policy: reviewed data is writable and all sections are readable.
    const auto section_offset = optional_offset + file.SizeOfOptionalHeader;
    for (std::size_t index = 0; index < profile.sections.size(); ++index) {
        IMAGE_SECTION_HEADER section{};
        std::memcpy(section.Name, profile.sections[index].name.data(), profile.sections[index].name.size());
        section.VirtualAddress = profile.sections[index].virtual_address;
        section.Misc.VirtualSize = profile.sections[index].virtual_size;
        section.Characteristics = IMAGE_SCN_MEM_READ;
        if (profile.sections[index].name == ".text") {
            section.Characteristics |= IMAGE_SCN_MEM_EXECUTE;
        }
        if (profile.sections[index].name == ".data") {
            section.Characteristics |= IMAGE_SCN_MEM_WRITE;
        }
        write_at(image, section_offset + index * sizeof(section), section);
    }
    return image;
}

} // namespace

TEST_CASE("every reviewed target profile is recognized by exact PE facts") {
    for (const auto& profile : fc::targets::known_image_profiles()) {
        auto image = mapped_image(profile);
        for (const auto basename : profile.basenames) {
            const auto recognized = fc::targets::recognize_mapped_image(basename, image);
            INFO("Profile: " << profile.id << ", basename: " << basename);
            REQUIRE(recognized.has_value());
            CHECK(std::string{recognized->profile->id} == std::string{profile.id});
            CHECK(recognized->profile->image == profile.image);
        }
    }
}

TEST_CASE("image profile IDs match case-insensitively and return canonical identity") {
    const auto& expected = fc::targets::known_image_profiles().front();
    std::string alternate_case{expected.id};
    std::ranges::transform(alternate_case, alternate_case.begin(), [](char value) {
        return value >= 'a' && value <= 'z'
                   ? static_cast<char>(value - ('a' - 'A'))
                   : (value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A')) : value);
    });

    // Lookup accepts configuration-style case variation without changing the reviewed identity exposed to callers.
    const auto* found = fc::targets::find_image_profile(alternate_case);
    REQUIRE(found != nullptr);
    CHECK(std::string{found->id} == std::string{expected.id});
}

TEST_CASE("recognition rejects malformed, unknown, and ambiguous images") {
    const auto& profile = fc::targets::known_image_profiles().front();
    auto image = mapped_image(profile);
    CHECK(fc::targets::recognize_mapped_image("Unknown.exe", image).error().kind ==
          fc::targets::RecognitionErrorKind::Unsupported);
    CHECK(fc::targets::recognize_mapped_image("BattlefrontII.exe", std::span{image}.first(32)).error().kind ==
          fc::targets::RecognitionErrorKind::MalformedImage);

    // Supplying a duplicated profile proves matching considers the complete catalog instead of accepting first-match.
    std::array duplicated{profile, profile};
    duplicated[1].id = "Duplicate_Profile";
    CHECK(fc::targets::recognize_mapped_image(profile.basenames.front(), image, duplicated).error().kind ==
          fc::targets::RecognitionErrorKind::Ambiguous);
}

TEST_CASE("mapped ownership keeps stable profile facts and enforces reviewed writable sections") {
    const auto& profile = fc::targets::known_image_profiles().front();
    auto owned = fc::targets::recognize_owned_mapped_image(profile.basenames.front(), mapped_image(profile));
    REQUIRE(owned.has_value());
    // Section policy, not mere backing storage, separates executable text from writable data.
    CHECK(std::string{owned->view().info().profile} == std::string{profile.id});
    CHECK_FALSE(owned->view().is_writable({profile.sections.front().virtual_address}, 1));
    CHECK(owned->view().is_executable({profile.sections.front().virtual_address}, 1));
    CHECK(owned->view().is_writable({profile.sections.back().virtual_address}, 1));
    CHECK_FALSE(owned->view().is_executable({profile.sections.back().virtual_address}, 1));
    std::array<std::byte, 2> header{};
    CHECK(owned->view().read({0}, header).has_value());

    std::vector<fc::targets::OwnedImage> images;
    images.push_back(std::move(*owned));
    auto target = fc::targets::RecognizedTarget::create(FC_HOST_ROLE_CLIENT, std::move(images));
    REQUIRE(target.has_value());
    CHECK(target->layout() == profile.layout);
    CHECK(std::string{target->info(FC_IMAGE_GAME).image_profile} == std::string{profile.id});
    CHECK(target->find(FC_IMAGE_GALAXY_PEER) == nullptr);

    // Reads cannot straddle the recognized image extent even when the destination buffer itself is valid.
    std::array<std::byte, 2> output{};
    CHECK_FALSE(target->find(FC_IMAGE_GAME)->read({profile.size_of_image - 1}, output).has_value());
}

TEST_CASE("recognized targets enforce startup image combinations and ownership of late images") {
    const auto profiles = fc::targets::known_image_profiles();
    const auto find = [&](std::string_view id) -> const fc::targets::ImageProfile& {
        const auto profile = std::ranges::find_if(profiles, [&](const auto& candidate) {
            return candidate.id == id;
        });
        REQUIRE(profile != profiles.end());
        return *profile;
    };
    const auto owned = [](const fc::targets::ImageProfile& profile) {
        std::vector<std::byte> bytes(1);
        return fc::targets::OwnedImage::mapped({profile.image, profile.id, 0, bytes.size()}, std::move(bytes), {});
    };

    // A Classic client requires both physical images before its target can be published.
    const auto& bootstrap = find("ClassicCollection_Bootstrap_66702CD7");
    const auto& classic_game = find("ClassicCollection_Game_66702CD2");
    std::vector<fc::targets::OwnedImage> incomplete;
    incomplete.push_back(owned(bootstrap));
    CHECK_FALSE(fc::targets::RecognizedTarget::create(FC_HOST_ROLE_CLIENT, std::move(incomplete)).has_value());

    std::vector<fc::targets::OwnedImage> classic_images;
    classic_images.push_back(owned(bootstrap));
    classic_images.push_back(owned(classic_game));
    auto classic = fc::targets::RecognizedTarget::create(FC_HOST_ROLE_CLIENT, std::move(classic_images));
    REQUIRE(classic.has_value());
    CHECK(classic->find(FC_IMAGE_BOOTSTRAP) != nullptr);
    CHECK(classic->find(FC_IMAGE_GAME) != nullptr);

    // The server process is the reviewed Game image itself and therefore has no client Bootstrap companion.
    std::vector<fc::targets::OwnedImage> classic_server_images;
    classic_server_images.push_back(owned(classic_game));
    auto classic_server = fc::targets::RecognizedTarget::create(FC_HOST_ROLE_SERVER, std::move(classic_server_images));
    REQUIRE(classic_server.has_value());
    CHECK(classic_server->find(FC_IMAGE_BOOTSTRAP) == nullptr);
    CHECK(classic_server->find(FC_IMAGE_GAME) != nullptr);

    // A reviewed image cannot be combined with a role that has no production deployment for its layout.
    const auto& mod_tools = find("ModTools_Game_43EBD102");
    std::vector<fc::targets::OwnedImage> invalid_role;
    invalid_role.push_back(owned(mod_tools));
    CHECK_FALSE(fc::targets::RecognizedTarget::create(FC_HOST_ROLE_SERVER, std::move(invalid_role)).has_value());

    // The same late GalaxyPeer owner is admissible for a GOG server but never for a client.
    const auto& gog_game = find("GOGRetail_Game_59EDF52B");
    const auto& galaxy_peer = find("GOGRetail_GalaxyPeer_59E6304A");
    std::vector<fc::targets::OwnedImage> server_images;
    server_images.push_back(owned(gog_game));
    auto server = fc::targets::RecognizedTarget::create(FC_HOST_ROLE_SERVER, std::move(server_images));
    REQUIRE(server.has_value());
    // The production probe must classify a genuinely unloaded reviewed DLL as absence, not permanent rejection.
    auto absent = fc::targets::probe_late_image(*server, FC_IMAGE_GALAXY_PEER);
    REQUIRE(absent.has_value());
    CHECK_FALSE(absent->has_value());
    CHECK(server->add_late_image(owned(galaxy_peer)));
    CHECK(server->find(FC_IMAGE_GALAXY_PEER) != nullptr);

    std::vector<fc::targets::OwnedImage> client_images;
    client_images.push_back(owned(gog_game));
    auto client = fc::targets::RecognizedTarget::create(FC_HOST_ROLE_CLIENT, std::move(client_images));
    REQUIRE(client.has_value());
    CHECK_FALSE(fc::targets::probe_late_image(*client, FC_IMAGE_GALAXY_PEER).has_value());
    CHECK_FALSE(client->add_late_image(owned(galaxy_peer)));
}
