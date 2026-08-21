#include "relocated_plan.hpp"
#include "target_image_fixture.hpp"

#include <catch2/catch_test_macros.hpp>

#include <Windows.h>

#include <process.h>

#include <atomic>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

// The verifier receives real files in an isolated directory so its non-mutation guarantee can be checked exactly.
class TemporaryDirectory final {
  public:
    TemporaryDirectory() {
        static std::atomic_uint64_t sequence{};
        path = std::filesystem::temp_directory_path() /
               ("FusionCutter-RelocatedVerifier-" + std::to_string(GetCurrentProcessId()) + "-" +
                std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)));
        REQUIRE(std::filesystem::create_directory(path));
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }

    std::filesystem::path path;
};

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

} // namespace

TEST_CASE("relocated plan verifier accepts its default nonconflicting patch set without modifying images") {
    TemporaryDirectory directory;
    std::vector<std::pair<std::filesystem::path, std::vector<std::byte>>> inputs;
    std::vector<std::wstring> arguments{L"--role", L"client"};
#if defined(_M_IX86)
    const auto* game = fc::targets::find_image_profile("SteamRetail_Game_59EDE353");
#else
    const auto* bootstrap = fc::targets::find_image_profile("ClassicCollection_Bootstrap_66702CD7");
    REQUIRE(bootstrap != nullptr);
    auto bootstrap_bytes = fc::fixtures::raw_target_image(*bootstrap);
    const auto bootstrap_path = directory.path / std::filesystem::path{bootstrap->basenames.front()};
    write_file(bootstrap_path, bootstrap_bytes);
    inputs.emplace_back(bootstrap_path, bootstrap_bytes);
    arguments.insert(arguments.end(), {L"--image", L"Bootstrap=" + bootstrap_path.native()});
    const auto* game = fc::targets::find_image_profile("ClassicCollection_Game_66702CD2");
#endif
    REQUIRE(game != nullptr);
    auto game_bytes = fc::fixtures::raw_target_image(*game);
    const auto game_path = directory.path / std::filesystem::path{game->basenames.front()};
    write_file(game_path, game_bytes);
    inputs.emplace_back(game_path, game_bytes);
    arguments.insert(arguments.end(), {L"--image", L"Game=" + game_path.native(), L"--plugin", FC_RELOCATED_PLUGIN_PATH,
                                       L"--check", L"RelocatedStorage", L"--check", L"RelocatedIndependent"});

    // Named checks prove the intended patches reached Ready before verifying that every source file remains unchanged.
    CHECK(run_verifier(std::move(arguments)) == 0);
    for (const auto& [path, expected] : inputs) {
        std::ifstream input{path, std::ios::binary};
        const std::vector<char> actual{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
        REQUIRE(actual.size() == expected.size());
        CHECK(std::memcmp(actual.data(), expected.data(), expected.size()) == 0);
    }
}
