#include "core_runtime.hpp"
#include <Windows.h>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace fc::runtime;

// Owns section names and profiles whose borrowed views are retained by the child runtime.
struct Catalog {
    std::vector<std::string> names;
    std::vector<SectionProfile> sections;
    std::array<std::string_view, 1> filenames;
    std::array<ImageProfile, 1> profiles;

    // Captures a valid loaded PE module's headers; filename and identity storage must outlive this catalog.
    Catalog(HMODULE module = GetModuleHandleW(nullptr), std::string_view filename = "fc_control_child.exe",
            std::string_view identity = "ApiFixture")
        : filenames{filename} {
        const auto base = reinterpret_cast<const std::byte *>(module);
        const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
        const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
        const auto table = IMAGE_FIRST_SECTION(nt);

        // Reserve name storage before storing views into it so vector growth cannot invalidate section names.
        names.reserve(nt->FileHeader.NumberOfSections);
        sections.reserve(nt->FileHeader.NumberOfSections);
        for (unsigned i = 0; i != nt->FileHeader.NumberOfSections; ++i) {
            const auto &entry = table[i];
            const auto size = strnlen(reinterpret_cast<const char *>(entry.Name), 8);
            names.emplace_back(reinterpret_cast<const char *>(entry.Name), size);
            sections.push_back({names.back(), entry.VirtualAddress, entry.Misc.VirtualSize});
        }

        profiles[0] = {identity, filenames, nt->OptionalHeader.SizeOfImage, sections};
    }
};

// Attempts a byte write at address and catches its access violation so the child can inspect the crash report.
void caught_write(void *address) noexcept {
    __try {
        *static_cast<volatile std::uint8_t *>(address) = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// Reads a fixture result file; missing or empty output produces an empty string for the polling loops.
std::string read(const std::filesystem::path &path) {
    std::ifstream input{path};
    return {std::istreambuf_iterator<char>{input}, {}};
}
} // namespace

// Runs one runtime scenario with real plugin DLLs; returns 0 after observing its expected output.
// Fatal callback scenarios rely on runtime termination before the fallback timeout return.
int main(int argc, char **argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    if (argc != 2) {
        return 2;
    }

    const std::string mode{argv[1]};
    // Expose the selected mode to the provider and consumer DLLs before runtime startup.
    {
        std::ofstream request{"test.mode"};
        request << mode;
    }

    const bool image_case = mode == "image-late" || mode == "timing-late" || mode == "late-settings" || mode == "image-incompatible";
    HMODULE game{};
    if (image_case) {
        std::filesystem::copy_file(FC_IMAGE_FIXTURE, "catalog-game.dll");
        game = LoadLibraryW(L"catalog-game.dll");
        if (!game) {
            return 13;
        }
    }

    auto *catalog = new Catalog;
    auto *game_catalog = image_case ? new Catalog(game, "catalog-game.dll", "GameFixture") : nullptr;
    auto *profiles = new std::vector<ImageProfile>{catalog->profiles.begin(), catalog->profiles.end()};
    if (game_catalog) {
        profiles->push_back(game_catalog->profiles.front());
    }
    if (mode == "image-incompatible") {
        // Alter the expected section size so the real DLL fails recognition when loaded later.
        ++game_catalog->sections[0].size;
    }

    // Unload the fixture after reading its profile so runtime startup observes it as absent.
    if (image_case) {
        FreeLibrary(game);
        game = nullptr;
    }

    if (mode == "late-settings") {
        std::filesystem::copy_file(FC_IMAGE_FIXTURE, "catalog-peer.dll");
        const auto peer = LoadLibraryW(L"catalog-peer.dll");
        if (!peer) {
            return 12;
        }
        auto *peer_catalog = new Catalog(peer, "catalog-peer.dll", "PeerFixture");
        profiles->push_back(peer_catalog->profiles.front());
        FreeLibrary(peer);
        // Capture both settings sources before startup; later changes must not affect the waiting factory.
        std::ofstream{"FC.ApiProvider.ini"} << "[Publish]\nfrom_ini=23\nfrom_environment=25\n";
        SetEnvironmentVariableW(L"FC_LATE_CAPTURE", L"31");
    }

    // Retain the runtime and its borrowed catalog storage until the child process exits.
    auto *runtime = new CoreRuntime(*profiles);
    if (!runtime->start(FC_ROLE_CLIENT, {})) {
        return 3;
    }

    if (image_case) {
        if (mode == "late-settings") {
            // Change both sources while the provider still waits; the independent observer must continue updating.
            std::ofstream{"FC.ApiProvider.ini"} << "[Publish]\nfrom_ini=99\nfrom_environment=99\n";
            SetEnvironmentVariableW(L"FC_LATE_CAPTURE", L"99");
            for (unsigned i = 0; i != 600 && read("progress.result").empty(); ++i) {
                Sleep(10);
            }
            if (read("progress.result").empty() || std::filesystem::exists("factory.result")) {
                return 19;
            }
        }

        game = LoadLibraryW(L"catalog-game.dll");
        if (!game) {
            return 14;
        }

        if (mode == "late-settings") {
            // One required image is loaded; the handler must wait for the second while independent updates continue.
            const auto before = read("progress.result");
            Sleep(200);
            if (std::filesystem::exists("factory.result") || read("progress.result") == before) {
                return 20;
            }
            if (!LoadLibraryW(L"catalog-peer.dll")) {
                return 21;
            }
        }

        // Wait for selection and reporting to observe the newly loaded image, then check this case's result.
        for (unsigned i = 0; i != 600; ++i) {
            const auto status = read("FusionCutter.txt");
            if (mode == "image-incompatible") {
                if (status.find("ApiProvider.Publish: Failed") != std::string::npos && !read("progress.result").empty()) {
                    return status.find("no matching entry") != std::string::npos ? 0 : 22;
                }
            } else if (status.find("ApiProvider 1: 2 installed") != std::string::npos) {
                if (mode == "timing-late") {
                    // The observer compares its update timestamp with the provider's preparation completion time.
                    const auto timing = read("timing.result");
                    if (timing.empty()) {
                        Sleep(10);
                        continue;
                    }
                    return timing == "fresh" ? 0 : 18;
                }
                if (mode == "late-settings") {
                    Sleep(200);
                    return read("factory.result") == "23 31\n" ? 0 : 23;
                }

                const auto base = reinterpret_cast<std::uintptr_t>(game);
                caught_write(reinterpret_cast<void *>(base + 4));
                const auto output = read("FusionCutter.crash.log");
                return output.find("catalog-game.dll+0x4, timestamp=") != std::string::npos &&
                               output.find("GameFixture") != std::string::npos
                           ? 0
                           : 15;
            }
            Sleep(10);
        }
        return 17;
    }

    if (mode == "consumer-fault") {
        // The installed provider DLL invokes the real consumer binding on its own thread after this request.
        {
            std::ofstream request{"fault.request"};
            request << mode;
        }
        Sleep(6000);
        return 5;
    }
    if (mode == "explicit-fault") {
        Sleep(6000);
        return 5;
    }

    // Availability notifications follow activation; wait for update and status callbacks to record their stages too.
    const auto wanted = mode == "valid" ? "DLL API value=42 available=15" : "ApiConsumer.Read: Failed";
    for (unsigned i = 0; i != 600; ++i) {
        const auto status = read("FusionCutter.txt");
        if (status.find(wanted) != std::string::npos) {
            if (mode == "valid") {
                if (status.find("Control callbacks: same_thread=true stages=127") == std::string::npos) {
                    Sleep(10);
                    continue;
                }
                if (status.find("ApiProvider 1: 1 installed") == std::string::npos) {
                    return 6;
                }
                if (runtime->start(FC_ROLE_CLIENT, {}) != 1 || runtime->start(FC_ROLE_SERVER, {}) != 0) {
                    return 7;
                }
            } else if (status.find("incompatible contract") == std::string::npos ||
                       status.find("ApiProvider 1: 1 installed") == std::string::npos) {
                return 8;
            }
            return 0;
        }
        Sleep(10);
    }
    return 9;
}
