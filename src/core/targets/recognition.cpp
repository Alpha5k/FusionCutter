#include "recognition.hpp"

#include <Windows.h>
#include <Psapi.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstring>
#include <limits>
#include <ranges>
#include <utility>

namespace fc::targets {
namespace {

// Owns the bounded PE identity and section access facts needed to match one reviewed image profile.
struct PeFacts {
    FC_Architecture architecture{};
    std::uint32_t timestamp{};
    std::uint32_t size_of_image{};
    std::uint32_t size_of_headers{};
    std::vector<IMAGE_SECTION_HEADER> sections;
};

[[nodiscard]] constexpr char ascii_lower(char value) noexcept {
    return value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A')) : value;
}

[[nodiscard]] bool ascii_iequals(std::string_view left, std::string_view right) noexcept {
    return left.size() == right.size() && std::ranges::equal(left, right, {}, ascii_lower, ascii_lower);
}

template <class Value>
[[nodiscard]] std::optional<Value> read_at(std::span<const std::byte> image, std::size_t offset) noexcept {
    if (offset > image.size() || sizeof(Value) > image.size() - offset) {
        return std::nullopt;
    }
    Value value{};
    std::memcpy(&value, image.data() + offset, sizeof(value));
    return value;
}

// Extracts only the bounded fingerprints and memory access facts needed to recognize a mapped PE image.
[[nodiscard]] std::expected<PeFacts, RecognitionError> inspect_pe(std::span<const std::byte> image) {
    // Copies avoid unaligned reads and keep malformed mapped images inside ordinary bounds-checked failure paths.
    const auto dos = read_at<IMAGE_DOS_HEADER>(image, 0);
    if (!dos || dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0) {
        return std::unexpected(
            RecognitionError{RecognitionErrorKind::MalformedImage, "Invalid DOS header", "Recognize target image"});
    }
    const auto nt_offset = static_cast<std::size_t>(dos->e_lfanew);
    const auto signature = read_at<DWORD>(image, nt_offset);
    const auto file = read_at<IMAGE_FILE_HEADER>(image, nt_offset + sizeof(DWORD));
    if (!signature || *signature != IMAGE_NT_SIGNATURE || !file) {
        return std::unexpected(
            RecognitionError{RecognitionErrorKind::MalformedImage, "Invalid PE header", "Recognize target image"});
    }

    const auto optional_offset = nt_offset + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER);
    const auto magic = read_at<WORD>(image, optional_offset);
    if (!magic) {
        return std::unexpected(RecognitionError{RecognitionErrorKind::MalformedImage, "Missing optional header",
                                                "Recognize target image"});
    }

    // Machine and optional header kind must agree before architecture-dependent fields are read.
    PeFacts facts;
    std::uint32_t size_of_headers{};
    if (*magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC && file->Machine == IMAGE_FILE_MACHINE_I386 &&
        file->SizeOfOptionalHeader >= sizeof(IMAGE_OPTIONAL_HEADER32)) {
        const auto optional = read_at<IMAGE_OPTIONAL_HEADER32>(image, optional_offset);
        if (!optional) {
            return std::unexpected(RecognitionError{RecognitionErrorKind::MalformedImage, "Truncated PE32 header",
                                                    "Recognize target image"});
        }
        facts.architecture = FC_ARCH_X86;
        facts.size_of_image = optional->SizeOfImage;
        size_of_headers = optional->SizeOfHeaders;
    } else if (*magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC && file->Machine == IMAGE_FILE_MACHINE_AMD64 &&
               file->SizeOfOptionalHeader >= sizeof(IMAGE_OPTIONAL_HEADER64)) {
        const auto optional = read_at<IMAGE_OPTIONAL_HEADER64>(image, optional_offset);
        if (!optional) {
            return std::unexpected(RecognitionError{RecognitionErrorKind::MalformedImage, "Truncated PE32+ header",
                                                    "Recognize target image"});
        }
        facts.architecture = FC_ARCH_X64;
        facts.size_of_image = optional->SizeOfImage;
        size_of_headers = optional->SizeOfHeaders;
    } else {
        return std::unexpected(RecognitionError{RecognitionErrorKind::MalformedImage,
                                                "PE machine and optional header kind disagree",
                                                "Recognize target image"});
    }
    if (facts.size_of_image == 0 || facts.size_of_image > image.size() || size_of_headers > facts.size_of_image) {
        return std::unexpected(RecognitionError{RecognitionErrorKind::MalformedImage, "Invalid mapped image bounds",
                                                "Recognize target image"});
    }
    facts.timestamp = file->TimeDateStamp;
    facts.size_of_headers = size_of_headers;

    // The section table must lie within the copied headers and every declared virtual range within SizeOfImage.
    const auto sections_offset = optional_offset + file->SizeOfOptionalHeader;
    if (sections_offset > size_of_headers ||
        file->NumberOfSections > (size_of_headers - sections_offset) / sizeof(IMAGE_SECTION_HEADER)) {
        return std::unexpected(RecognitionError{RecognitionErrorKind::MalformedImage, "Invalid PE section table",
                                                "Recognize target image"});
    }
    facts.sections.reserve(file->NumberOfSections);
    for (std::size_t index = 0; index < file->NumberOfSections; ++index) {
        const auto section =
            read_at<IMAGE_SECTION_HEADER>(image, sections_offset + index * sizeof(IMAGE_SECTION_HEADER));
        if (!section || section->VirtualAddress > facts.size_of_image ||
            section->Misc.VirtualSize > facts.size_of_image - section->VirtualAddress) {
            return std::unexpected(RecognitionError{RecognitionErrorKind::MalformedImage, "Invalid PE section bounds",
                                                    "Recognize target image"});
        }
        facts.sections.push_back(*section);
    }
    return facts;
}

[[nodiscard]] std::string_view section_name(const IMAGE_SECTION_HEADER& section) noexcept {
    const auto* begin = reinterpret_cast<const char*>(section.Name);
    const auto* end = std::find(begin, begin + IMAGE_SIZEOF_SHORT_NAME, '\0');
    return {begin, static_cast<std::size_t>(end - begin)};
}

[[nodiscard]] bool profile_matches(std::string_view basename, const PeFacts& facts,
                                   const ImageProfile& profile) noexcept {
    // Profile matching uses only reviewed layout facts; patch-specific byte evidence is intentionally absent here.
    if (facts.architecture != profile.architecture || facts.size_of_image != profile.size_of_image ||
        (profile.timestamp && facts.timestamp != *profile.timestamp) ||
        !std::ranges::any_of(profile.basenames, [&](std::string_view name) {
            return ascii_iequals(basename, name);
        })) {
        return false;
    }
    return std::ranges::all_of(profile.sections, [&](const SectionProfile& expected) {
        return std::ranges::count_if(facts.sections, [&](const IMAGE_SECTION_HEADER& section) {
                   return section_name(section) == expected.name &&
                          section.VirtualAddress == expected.virtual_address &&
                          section.Misc.VirtualSize == expected.virtual_size;
               }) == 1;
    });
}

// Both recognition and explicit Scenario validation derive access policy from the bytes they actually received.
[[nodiscard]] RecognizedImageFacts image_facts(const ImageProfile& profile, const PeFacts& facts) {
    RecognizedImageFacts result{.profile = &profile};
    result.readable_ranges.emplace_back(0, facts.size_of_headers);
    for (const auto& section : facts.sections) {
        if ((section.Characteristics & IMAGE_SCN_MEM_WRITE) != 0) {
            result.writable_ranges.emplace_back(section.VirtualAddress, section.Misc.VirtualSize);
        }
        if ((section.Characteristics & IMAGE_SCN_MEM_READ) != 0) {
            result.readable_ranges.emplace_back(section.VirtualAddress, section.Misc.VirtualSize);
        }
        if ((section.Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0) {
            result.executable_ranges.emplace_back(section.VirtualAddress, section.Misc.VirtualSize);
        }
    }
    return result;
}

// Recognizes one already-referenced live module and transfers that reference into its returned owner.
[[nodiscard]] std::expected<OwnedImage, RecognitionError> recognize_live_module(HMODULE module) {
    // Establish the complete mapped extent and overflow-safe address range before forming a byte span.
    MODULEINFO module_info{};
    if (module == nullptr ||
        K32GetModuleInformation(GetCurrentProcess(), module, &module_info, sizeof(module_info)) == 0 ||
        module_info.lpBaseOfDll == nullptr || module_info.SizeOfImage == 0) {
        return std::unexpected(RecognitionError{RecognitionErrorKind::System, "Loaded image information is unavailable",
                                                "Recognize target image"});
    }
    const auto base = reinterpret_cast<std::uintptr_t>(module_info.lpBaseOfDll);
    if (module_info.SizeOfImage > std::numeric_limits<std::uintptr_t>::max() - base) {
        return std::unexpected(
            RecognitionError{RecognitionErrorKind::System, "Loaded image bounds overflow", "Recognize target image"});
    }
    // The loader's basename participates in the fingerprint but never supplies an unbounded path string.
    std::array<char, MAX_PATH> basename{};
    const auto size =
        K32GetModuleBaseNameA(GetCurrentProcess(), module, basename.data(), static_cast<DWORD>(basename.size()));
    if (size == 0 || size >= basename.size()) {
        return std::unexpected(RecognitionError{RecognitionErrorKind::System, "Loaded image basename is unavailable",
                                                "Recognize target image"});
    }
    const auto bytes = std::span{static_cast<const std::byte*>(module_info.lpBaseOfDll),
                                 static_cast<std::size_t>(module_info.SizeOfImage)};
    auto recognized = recognize_mapped_image({basename.data(), size}, bytes);
    if (!recognized) {
        return std::unexpected(std::move(recognized.error()));
    }
    ImageInfo info{recognized->profile->image, recognized->profile->id, base, module_info.SizeOfImage};
    return OwnedImage::live(info, module, std::move(recognized->writable_ranges),
                            std::move(recognized->readable_ranges), std::move(recognized->executable_ranges));
}

[[nodiscard]] std::expected<OwnedImage, RecognitionError> acquire_and_recognize(const wchar_t* module_name) {
    HMODULE module{};
    // This reference is acquired before any view is formed and is transferred directly to OwnedImage.
    if (GetModuleHandleExW(0, module_name, &module) == 0 || module == nullptr) {
        return std::unexpected(RecognitionError{RecognitionErrorKind::System, "Required target image is not loaded",
                                                "Acquire target image"});
    }
    auto result = recognize_live_module(module);
    if (!result) {
        FreeLibrary(module);
    }
    return result;
}

} // namespace

std::expected<RecognizedImageFacts, RecognitionError> recognize_mapped_image(std::string_view basename,
                                                                             std::span<const std::byte> mapped_image,
                                                                             std::span<const ImageProfile> profiles) {
    auto facts = inspect_pe(mapped_image);
    if (!facts) {
        return std::unexpected(std::move(facts.error()));
    }
    // Match the complete supplied profile set so duplicated fingerprints fail as ambiguous rather than first-wins.
    const ImageProfile* match{};
    for (const auto& profile : profiles) {
        if (!profile_matches(basename, *facts, profile)) {
            continue;
        }
        if (match != nullptr) {
            return std::unexpected(RecognitionError{RecognitionErrorKind::Ambiguous,
                                                    "Target image matches more than one reviewed profile",
                                                    "Recognize target image"});
        }
        match = &profile;
    }
    if (match == nullptr) {
        return std::unexpected(RecognitionError{RecognitionErrorKind::Unsupported,
                                                "Target image does not match a reviewed profile",
                                                "Recognize target image"});
    }

    // Access policy is derived from the validated PE headers, while exact identity comes from the reviewed profile.
    return image_facts(*match, *facts);
}

std::expected<OwnedImage, RecognitionError> recognize_owned_mapped_image(std::string_view basename,
                                                                         std::vector<std::byte> mapped_image,
                                                                         std::span<const ImageProfile> profiles) {
    auto recognized = recognize_mapped_image(basename, mapped_image, profiles);
    if (!recognized) {
        return std::unexpected(std::move(recognized.error()));
    }
    mapped_image.resize(recognized->profile->size_of_image);
    ImageInfo info{recognized->profile->image, recognized->profile->id, 0, mapped_image.size()};
    return OwnedImage::mapped(info, std::move(mapped_image), std::move(recognized->writable_ranges),
                              std::move(recognized->readable_ranges), std::move(recognized->executable_ranges));
}

std::expected<OwnedImage, RecognitionError> validate_synthetic_mapped_image(const ImageProfile& profile,
                                                                            std::vector<std::byte> mapped_image) {
    // PE inspection supplies trustworthy bounds and section access policy without performing fingerprint recognition.
    auto facts = inspect_pe(mapped_image);
    if (!facts) {
        return std::unexpected(std::move(facts.error()));
    }
    // Synthetic identity is explicit, but machine kind remains an objective property of the supplied PE bytes.
    if (facts->architecture != profile.architecture) {
        return std::unexpected(RecognitionError{RecognitionErrorKind::MalformedImage,
                                                "Synthetic image machine does not match its selected profile",
                                                "Validate scenario image"});
    }
    auto validated = image_facts(profile, *facts);
    mapped_image.resize(facts->size_of_image);
    ImageInfo info{profile.image, profile.id, 0, mapped_image.size()};
    return OwnedImage::mapped(info, std::move(mapped_image), std::move(validated.writable_ranges),
                              std::move(validated.readable_ranges), std::move(validated.executable_ranges));
}

RecognizedTarget::RecognizedTarget(FC_TargetLayout layout, FC_HostRole role, FC_Architecture architecture) noexcept
    : layout_(layout), role_(role), architecture_(architecture) {}

FC_TargetLayout RecognizedTarget::layout() const noexcept {
    return layout_;
}

FC_HostRole RecognizedTarget::role() const noexcept {
    return role_;
}

FC_Architecture RecognizedTarget::architecture() const noexcept {
    return architecture_;
}

TargetInfo RecognizedTarget::info(FC_TargetImage image) const noexcept {
    const auto* selected = find(image);
    assert(selected != nullptr);
    return {layout_, role_, architecture_, selected->info().profile};
}

const ImageView* RecognizedTarget::find(FC_TargetImage image) const noexcept {
    const auto index = slot(image);
    return index && images_[*index] ? &images_[*index]->view() : nullptr;
}

bool RecognizedTarget::add_late_image(OwnedImage image) noexcept {
    // Late attachment accepts only a reviewed polled profile for this target's exact layout and architecture.
    const auto index = slot(image.view().info().image);
    const auto* profile = find_image_profile(image.view().info().profile);
    if (!index || images_[*index] || profile == nullptr || !profile->late || profile->image != FC_IMAGE_GALAXY_PEER ||
        profile->layout != layout_ || profile->architecture != architecture_ ||
        !valid_target_tuple(layout_, role_, profile->image)) {
        return false;
    }
    images_[*index] = std::move(image);
    return true;
}

std::expected<RecognizedTarget, RecognitionError> RecognizedTarget::create(FC_HostRole role,
                                                                           std::vector<OwnedImage> startup_images) {
    // The first startup profile establishes the target identity that every additional image must share.
    if ((role != FC_HOST_ROLE_CLIENT && role != FC_HOST_ROLE_SERVER) || startup_images.empty()) {
        return std::unexpected(RecognitionError{RecognitionErrorKind::MalformedImage,
                                                "Startup target image set is incomplete", "Build recognized target"});
    }
    const auto& first_profile = startup_images.front().view().info().profile;
    const auto* profile = find_image_profile(first_profile);
    if (profile == nullptr || profile->late) {
        return std::unexpected(RecognitionError{RecognitionErrorKind::MalformedImage,
                                                "Startup target image has no reviewed profile",
                                                "Build recognized target"});
    }
    // Every image must belong to one layout and architecture and occupy one unique physical image slot.
    RecognizedTarget result{profile->layout, role, profile->architecture};
    if (!valid_target_role(result.layout_, role)) {
        return std::unexpected(RecognitionError{RecognitionErrorKind::MalformedImage,
                                                "The target layout does not support the supplied role",
                                                "Build recognized target"});
    }
    for (auto& image : startup_images) {
        const auto* image_profile = find_image_profile(image.view().info().profile);
        const auto index = slot(image.view().info().image);
        if (image_profile == nullptr || image_profile->layout != result.layout_ ||
            image_profile->architecture != result.architecture_ || image_profile->late || !index ||
            result.images_[*index]) {
            return std::unexpected(RecognitionError{RecognitionErrorKind::MalformedImage,
                                                    "Startup target images do not form one supported target",
                                                    "Build recognized target"});
        }
        result.images_[*index] = std::move(image);
    }
    // Only the Classic client starts from a Bootstrap/Game pair; every server and other client starts from one Game.
    const bool classic_client = result.layout_ == FC_LAYOUT_CLASSIC_COLLECTION && role == FC_HOST_ROLE_CLIENT;
    if ((!classic_client && (result.find(FC_IMAGE_GAME) == nullptr || startup_images.size() != 1)) ||
        (classic_client && (result.find(FC_IMAGE_BOOTSTRAP) == nullptr || result.find(FC_IMAGE_GAME) == nullptr ||
                            startup_images.size() != 2))) {
        return std::unexpected(RecognitionError{RecognitionErrorKind::MalformedImage,
                                                "Startup target image set is incomplete", "Build recognized target"});
    }
    return result;
}

std::optional<std::size_t> RecognizedTarget::slot(FC_TargetImage image) noexcept {
    if (image == FC_IMAGE_GAME) {
        return 0;
    }
    if (image == FC_IMAGE_BOOTSTRAP) {
        return 1;
    }
    if (image == FC_IMAGE_GALAXY_PEER) {
        return 2;
    }
    return std::nullopt;
}

std::expected<RecognizedTarget, RecognitionError> recognize_target(FC_HostRole role) {
    // The executable determines whether startup is complete or requires the role-specific Classic game DLL.
    auto executable = acquire_and_recognize(nullptr);
    if (!executable) {
        return std::unexpected(std::move(executable.error()));
    }
    std::vector<OwnedImage> startup;
    const auto executable_image = executable->view().info().image;
    startup.push_back(std::move(*executable));
    if (executable_image == FC_IMAGE_BOOTSTRAP) {
        const auto* name = role == FC_HOST_ROLE_CLIENT ? L"Battlefront2.original.dll" : L"Battlefront2.dll";
        auto game = acquire_and_recognize(name);
        if (!game) {
            return std::unexpected(std::move(game.error()));
        }
        startup.push_back(std::move(*game));
    }
    return RecognizedTarget::create(role, std::move(startup));
}

LateProbeResult probe_late_image(const RecognizedTarget& target, FC_TargetImage image) {
    // A target tuple selects the detection name before consulting process modules; plugins cannot add polling names.
    const ImageProfile* detection_profile{};
    for (const auto& profile : known_image_profiles()) {
        if (profile.layout == target.layout() && profile.architecture == target.architecture() &&
            profile.image == image && profile.late) {
            detection_profile = &profile;
            break;
        }
    }
    if (target.role() != FC_HOST_ROLE_SERVER || detection_profile == nullptr ||
        detection_profile->late->module_name == nullptr) {
        return std::unexpected(
            LateProbeError{"The target has no approved profile for the late image", "Recognize late target image"});
    }

    // GetModuleHandleEx combines presence detection with the owning reference required before any PE view is formed.
    HMODULE module{};
    SetLastError(ERROR_SUCCESS);
    if (GetModuleHandleExW(0, detection_profile->late->module_name, &module) == 0 || module == nullptr) {
        const auto error = GetLastError();
        if (error == ERROR_MOD_NOT_FOUND || error == ERROR_DLL_NOT_FOUND) {
            return std::optional<OwnedImage>{};
        }
        return std::unexpected(
            LateProbeError{"The late target module could not be pinned (Windows error " + std::to_string(error) + ')',
                           "Acquire late target image"});
    }

    // Target recognition may select a reviewed build sharing the detection name; the active target closes the tuple.
    auto recognized = recognize_live_module(module);
    if (!recognized) {
        FreeLibrary(module);
        return std::unexpected(LateProbeError{std::move(recognized.error().message), "Recognize GalaxyPeer image"});
    }
    const auto* matched = find_image_profile(recognized->view().info().profile);
    if (matched == nullptr || !matched->late || matched->layout != target.layout() ||
        matched->architecture != target.architecture() || matched->image != image) {
        return std::unexpected(
            LateProbeError{"The loaded module does not match the active target tuple", "Recognize GalaxyPeer image"});
    }

    // The profile's mutation policy is enforced after the Plan callback so common validation never mutates GalaxyPeer.
    return std::optional<OwnedImage>{std::move(*recognized)};
}

} // namespace fc::targets
