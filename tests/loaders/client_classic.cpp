#include "child_support.hpp"

#include <thread>

namespace fc::tests::loaders {

int client(std::string_view scenario) {
    const bool battlefront1 = scenario.starts_with("bf1-");
    if (battlefront1) {
        scenario.remove_prefix(4);
    }
    const auto loader_path = battlefront1 ? FC_BF1_LOADER_PATH : FC_CLIENT_LOADER_PATH;
    const auto loader_name = battlefront1 ? L"Battlefront1.dll" : FC_CLIENT_LOADER_NAME;
    const auto original_name = battlefront1 ? L"Battlefront1.original.dll" : L"Battlefront2.original.dll";
    const auto directory = stage();
    if (const auto failure = prepare_client(directory, scenario, loader_path, loader_name)) {
        return failure;
    }

    if (scenario != "missing-original" && !copy_fixture(FC_CLIENT_AUXILIARY_PATH, directory / original_name)) {
        return 12;
    }

    const auto proxy = LoadLibraryW((directory / loader_name).c_str());
    if (!proxy) {
        return 13;
    }

    // Check the launcher's named and ordinal entry points before the original game DLL is loaded.
    using Entry =
        std::uint32_t (*)(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint8_t *, std::uint32_t, std::uint64_t);
    const auto entry = resolve<Entry>(proxy, "GameWinMain");
    if (!entry || GetProcAddress(proxy, MAKEINTRESOURCEA(1)) != reinterpret_cast<FARPROC>(entry) || GetModuleHandleW(original_name)) {
        return 14;
    }

    // Distinct values in the high bits reveal truncated or reordered arguments during forwarding.
    std::uint32_t first{}, second{};
    auto invoke = [&] {
        return entry(0x1234567800000001ull, 0x2345678900000002ull, 0x3456789A00000003ull, 0x456789AB00000004ull,
                     reinterpret_cast<std::uint8_t *>(0x1003), 0xFEDCBA98, 0x56789ABC00000006ull);
    };

    // Concurrent entry calls must share one runtime startup while both reach the original entry point.
    std::thread left([&] {
        first = invoke();
    }),
        right([&] {
            second = invoke();
        });
    left.join();
    right.join();

    const auto original = GetModuleHandleW(original_name);
    const auto valid = resolve<bool (*)()>(original, "FC_Test_OriginalValid");
    return first == 0x87654321 && second == first && valid && valid() && startup_captured() ? 0 : 15;
}

} // namespace fc::tests::loaders
