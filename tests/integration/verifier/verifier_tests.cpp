#include "targets/target_profiles.hpp"

#include <catch2/catch_test_macros.hpp>

#include <Windows.h>

#include <process.h>

#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

// Each verifier process receives isolated files that the fixture removes after mutation checks complete.
class TemporaryDirectory final {
  public:
    TemporaryDirectory() {
        static std::atomic_uint64_t sequence{};
        path = std::filesystem::temp_directory_path() /
               ("FusionCutter-Verifier-" + std::to_string(GetCurrentProcessId()) + "-" +
                std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)));
        REQUIRE(std::filesystem::create_directory(path));
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }

    std::filesystem::path path;
};

// Raw fixture sections need no payload: recognition uses virtual facts, and built-in Core plugin has no Plan callback.
[[nodiscard]] std::vector<std::byte> raw_image(const fc::targets::ImageProfile& profile) {
    // Build the on-disk DOS, COFF, and optional headers required by the verifier's production mapping path.
    std::vector<std::byte> file_bytes(0x400);
    IMAGE_DOS_HEADER dos{.e_magic = IMAGE_DOS_SIGNATURE, .e_lfanew = 0x80};
    std::memcpy(file_bytes.data(), &dos, sizeof(dos));
    const DWORD signature = IMAGE_NT_SIGNATURE;
    std::memcpy(file_bytes.data() + dos.e_lfanew, &signature, sizeof(signature));

    IMAGE_FILE_HEADER file{};
    file.Machine = profile.architecture == FC_ARCH_X86 ? IMAGE_FILE_MACHINE_I386 : IMAGE_FILE_MACHINE_AMD64;
    file.NumberOfSections = static_cast<WORD>(profile.sections.size());
    file.TimeDateStamp = profile.timestamp.value_or(0);
    file.SizeOfOptionalHeader =
        profile.architecture == FC_ARCH_X86 ? sizeof(IMAGE_OPTIONAL_HEADER32) : sizeof(IMAGE_OPTIONAL_HEADER64);
    const auto file_offset = static_cast<std::size_t>(dos.e_lfanew) + sizeof(DWORD);
    std::memcpy(file_bytes.data() + file_offset, &file, sizeof(file));
    const auto optional_offset = file_offset + sizeof(file);
    if (profile.architecture == FC_ARCH_X86) {
        IMAGE_OPTIONAL_HEADER32 optional{};
        optional.Magic = IMAGE_NT_OPTIONAL_HDR32_MAGIC;
        optional.SizeOfImage = profile.size_of_image;
        optional.SizeOfHeaders = static_cast<DWORD>(file_bytes.size());
        std::memcpy(file_bytes.data() + optional_offset, &optional, sizeof(optional));
    } else {
        IMAGE_OPTIONAL_HEADER64 optional{};
        optional.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
        optional.SizeOfImage = profile.size_of_image;
        optional.SizeOfHeaders = static_cast<DWORD>(file_bytes.size());
        std::memcpy(file_bytes.data() + optional_offset, &optional, sizeof(optional));
    }
    // Virtual section facts carry the reviewed fingerprint and access policy without adding irrelevant raw payloads.
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
        std::memcpy(file_bytes.data() + section_offset + index * sizeof(section), &section, sizeof(section));
    }
    return file_bytes;
}

// Writes the raw-PE fixture once; all subsequent verifier access is expected to remain read-only.
void write_file(const std::filesystem::path& path, const std::vector<std::byte>& bytes) {
    std::ofstream output{path, std::ios::binary};
    REQUIRE(output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())));
}

// Select one complete production startup tuple for the verifier's architecture.
[[nodiscard]] std::vector<const fc::targets::ImageProfile*> startup_profiles() {
    std::vector<const fc::targets::ImageProfile*> result;
    for (const auto& profile : fc::targets::known_image_profiles()) {
        if (profile.architecture != (sizeof(void*) == 4 ? FC_ARCH_X86 : FC_ARCH_X64) || profile.late) {
            continue;
        }
        if constexpr (sizeof(void*) == 4) {
            if (profile.layout == FC_LAYOUT_STEAM_RETAIL) {
                result.push_back(&profile);
                break;
            }
        } else if (profile.layout == FC_LAYOUT_CLASSIC_COLLECTION) {
            result.push_back(&profile);
        }
    }
    return result;
}

// Spawns the real command-line binary so argument handling and process exit semantics are covered together.
[[nodiscard]] int run_verifier(std::vector<std::wstring> arguments) {
    arguments.insert(arguments.begin(), FC_VERIFIER_PATH);
    std::vector<const wchar_t*> native;
    native.reserve(arguments.size() + 1);
    for (const auto& argument : arguments) {
        native.push_back(argument.c_str());
    }
    native.push_back(nullptr);
    return static_cast<int>(_wspawnv(_P_WAIT, FC_VERIFIER_PATH, native.data()));
}

} // namespace

TEST_CASE("verifier recognizes real files and leaves every input unchanged") {
    TemporaryDirectory directory;
    const auto profiles = startup_profiles();
    REQUIRE_FALSE(profiles.empty());
    std::vector<std::pair<std::filesystem::path, std::vector<std::byte>>> inputs;
    std::vector<std::wstring> arguments{L"--role", L"client"};
    for (const auto* profile : profiles) {
        const auto path = directory.path / std::filesystem::path{std::string{profile->basenames.front()}};
        auto bytes = raw_image(*profile);
        write_file(path, bytes);
        inputs.emplace_back(path, bytes);
        arguments.push_back(L"--image");
        const auto image_name = profile->image == FC_IMAGE_BOOTSTRAP ? L"Bootstrap" : L"Game";
        arguments.push_back(std::wstring{image_name} + L"=" + path.native());
    }

    CHECK(run_verifier(std::move(arguments)) == 0);
    for (const auto& [path, expected] : inputs) {
        std::ifstream input{path, std::ios::binary};
        const std::vector<char> actual{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
        // Convert through char iterators deliberately only for an exact byte-for-byte non-mutation comparison.
        REQUIRE(actual.size() == expected.size());
        CHECK(std::memcmp(actual.data(), expected.data(), expected.size()) == 0);
    }
}

TEST_CASE("verifier rejects duplicate image IDs before opening their paths") {
    const std::vector<std::wstring> arguments{
        L"--role", L"client", L"--image", L"Game=missing-a.exe", L"--image", L"Game=missing-b.exe"};
    CHECK(run_verifier(arguments) == 2);
}
