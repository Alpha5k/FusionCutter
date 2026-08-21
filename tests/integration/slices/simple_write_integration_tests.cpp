#include "simple_write.hpp"
#include "target_image_fixture.hpp"

#include <catch2/catch_test_macros.hpp>

#include <Windows.h>

#include <process.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace {

// Each slice case owns its isolated files and removes them only after all output comparisons finish.
class TemporaryDirectory final {
  public:
    TemporaryDirectory() {
        static std::atomic_uint64_t sequence{};
        path = std::filesystem::temp_directory_path() /
               ("FusionCutter-SimpleWriteVerifier-" + std::to_string(GetCurrentProcessId()) + "-" +
                std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)));
        REQUIRE(std::filesystem::create_directory(path));
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }

    std::filesystem::path path;
};

// Selects one shipping profile per architecture whose reviewed data section contains the slice value.
[[nodiscard]] const fc::targets::ImageProfile& verifier_profile() {
    for (const auto& profile : fc::targets::known_image_profiles()) {
#if defined(_M_IX86)
        if (profile.layout == FC_LAYOUT_STEAM_RETAIL && profile.image == FC_IMAGE_GAME) {
#else
        if (profile.layout == FC_LAYOUT_CLASSIC_COLLECTION && profile.image == FC_IMAGE_GAME) {
#endif
            return profile;
        }
    }
    throw std::runtime_error{"The verifier fixture profile is unavailable"};
}

void write_file(const std::filesystem::path& path, std::span<const std::byte> bytes) {
    std::ofstream output{path, std::ios::binary};
    REQUIRE(output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())));
}

// Converts owned arguments to the null-terminated array required by the child-process boundary.
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

[[nodiscard]] std::string read_text(const std::filesystem::path& path) {
    std::ifstream input{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

// Captures the child result and generated outputs needed to compare external and bundled acquisition.
struct RuntimeResult {
    int exit_code{};
    std::string status;
    std::string configuration;
};

// Stages an isolated host using the public ABI with either external or custom bundled acquisition.
[[nodiscard]] RuntimeResult run_runtime_slice(const std::filesystem::path& directory, bool bundled, bool override) {
    // Stage the same native host with only the physical plugin acquisition route changed.
    REQUIRE(std::filesystem::create_directory(directory));
    const auto host = directory / "FusionCutterSliceHost.exe";
    const auto core = directory / "FusionCutter.dll";
    REQUIRE(std::filesystem::copy_file(FC_SLICE_HOST_PATH, host));
    REQUIRE(std::filesystem::copy_file(bundled ? FC_SLICE_BUNDLED_CORE_PATH : FC_SLICE_EXTERNAL_CORE_PATH, core));
    if (!bundled) {
        REQUIRE(std::filesystem::create_directory(directory / "plugins"));
        REQUIRE(std::filesystem::copy_file(FC_SIMPLE_WRITE_PLUGIN_PATH,
                                           directory / "plugins" / "fc_simple_write_slice.dll"));
    }
    if (override) {
        // An existing user file exercises override parsing; otherwise production generation supplies the default.
        REQUIRE(std::filesystem::create_directory(directory / "config"));
        std::ofstream output{directory / "config" / "FC.SimpleWriteSlice.ini", std::ios::binary};
        REQUIRE(output << "[General]\r\nSimpleCheckedWrite=true\r\n\r\n[SimpleCheckedWrite]\r\nReplacement="
                       << fc::fixtures::simple_write::kOverrideReplacement << "\r\n");
    }

    // The private host recognizes its fixed profile, verifies the installed write, and exits.
    const auto exit_code = static_cast<int>(_wspawnl(_P_WAIT, host.c_str(), host.c_str(), nullptr));
    return {exit_code, read_text(directory / "FusionCutter.txt"),
            read_text(directory / "config" / "FC.SimpleWriteSlice.ini")};
}

} // namespace

TEST_CASE("simple checked-write verifier uses recognized files without modifying them") {
    TemporaryDirectory directory;
    const auto& game = verifier_profile();
    const auto rva =
        fc::fixtures::simple_write::value_rva({static_cast<fc::TargetLayout>(game.layout), fc::HostRole::Client,
                                               static_cast<fc::Architecture>(game.architecture), game.id});
    std::array<std::byte, sizeof(std::uint32_t)> payload{};
    std::memcpy(payload.data(), &fc::fixtures::simple_write::kInitialValue, payload.size());
    const std::array payloads{fc::fixtures::ImagePayload{rva, payload}};
    auto game_bytes = fc::fixtures::raw_target_image(game, payloads);
    const auto game_path = directory.path / std::filesystem::path{game.basenames.front()};
    write_file(game_path, game_bytes);

    std::vector<std::pair<std::filesystem::path, std::vector<std::byte>>> inputs{{game_path, game_bytes}};
    std::vector<std::wstring> arguments{L"--role", L"client"};
#if defined(_M_X64)
    const auto* bootstrap = fc::targets::find_image_profile("ClassicCollection_Bootstrap_66702CD7");
    REQUIRE(bootstrap != nullptr);
    auto bootstrap_bytes = fc::fixtures::raw_target_image(*bootstrap);
    const auto bootstrap_path = directory.path / std::filesystem::path{bootstrap->basenames.front()};
    write_file(bootstrap_path, bootstrap_bytes);
    inputs.emplace_back(bootstrap_path, bootstrap_bytes);
    arguments.insert(arguments.end(), {L"--image", L"Bootstrap=" + bootstrap_path.native()});
#endif
    arguments.insert(arguments.end(), {L"--image", L"Game=" + game_path.native(), L"--plugin",
                                       FC_SIMPLE_WRITE_PLUGIN_PATH, L"--check", L"SimpleCheckedWrite"});

    // The verifier must accept the recognized image while leaving every source file byte-for-byte unchanged.
    CHECK(run_verifier(std::move(arguments)) == 0);
    for (const auto& [path, expected] : inputs) {
        std::ifstream input{path, std::ios::binary};
        const std::vector<char> actual{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
        REQUIRE(actual.size() == expected.size());
        CHECK(std::memcmp(actual.data(), expected.data(), expected.size()) == 0);
    }
}

TEST_CASE("simple checked-write verifier reports changed evidence through the common validation result") {
    TemporaryDirectory directory;
    const auto& game = verifier_profile();
    const auto rva =
        fc::fixtures::simple_write::value_rva({static_cast<fc::TargetLayout>(game.layout), fc::HostRole::Client,
                                               static_cast<fc::Architecture>(game.architecture), game.id});
    const auto changed = fc::fixtures::simple_write::kInitialValue ^ 1U;
    std::array<std::byte, sizeof(changed)> payload{};
    std::memcpy(payload.data(), &changed, payload.size());
    const std::array payloads{fc::fixtures::ImagePayload{rva, payload}};
    const auto game_path = directory.path / std::filesystem::path{game.basenames.front()};
    write_file(game_path, fc::fixtures::raw_target_image(game, payloads));

    std::vector<std::wstring> arguments{L"--role", L"client"};
#if defined(_M_X64)
    const auto* bootstrap = fc::targets::find_image_profile("ClassicCollection_Bootstrap_66702CD7");
    REQUIRE(bootstrap != nullptr);
    const auto bootstrap_path = directory.path / std::filesystem::path{bootstrap->basenames.front()};
    write_file(bootstrap_path, fc::fixtures::raw_target_image(*bootstrap));
    arguments.insert(arguments.end(), {L"--image", L"Bootstrap=" + bootstrap_path.native()});
#endif
    arguments.insert(arguments.end(), {L"--image", L"Game=" + game_path.native(), L"--plugin",
                                       FC_SIMPLE_WRITE_PLUGIN_PATH, L"--check", L"SimpleCheckedWrite"});
    CHECK(run_verifier(std::move(arguments)) == 1);
}

TEST_CASE("simple checked-write installs and reports identically through external and bundled acquisition") {
    TemporaryDirectory directory;
    for (const bool override : {false, true}) {
        const auto external =
            run_runtime_slice(directory.path / (override ? "external-override" : "external-default"), false, override);
        const auto bundled =
            run_runtime_slice(directory.path / (override ? "bundled-override" : "bundled-default"), true, override);
        CAPTURE(override);
        REQUIRE(external.exit_code == 0);
        REQUIRE(bundled.exit_code == 0);

        // Both physical routes retain the same logical identity, target support, result, and live status value.
        const auto replacement = std::to_string(override ? fc::fixtures::simple_write::kOverrideReplacement
                                                         : fc::fixtures::simple_write::kDefaultReplacement);
        for (const auto* result : {&external, &bundled}) {
            CHECK(result->status.find("Initialization: Completed") != std::string::npos);
#if defined(_M_IX86)
            CHECK(result->status.find("Target: GOG Retail (Server, x86)") != std::string::npos);
#else
            CHECK(result->status.find("Target: Classic Collection (Server, x64)") != std::string::npos);
#endif
            CHECK(result->status.find("SimpleWriteSlice (1.0.0)") != std::string::npos);
            CHECK(result->status.find("Simple checked write (SimpleWriteSlice/SimpleCheckedWrite)") !=
                  std::string::npos);
            CHECK(result->status.find("Replacement: " + replacement) != std::string::npos);
            CHECK(result->configuration.find("Replacement=" + replacement) != std::string::npos);
        }
    }
}
