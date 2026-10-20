# Your first game patch

This tutorial builds a plugin that changes Battlefront II's frame rate limit from 80 to 120 FPS on the
supported Steam and GOG builds. It connects the plugin declarations from [your first plugin](getting_started.md)
to a real change in game code: choosing builds, locating an instruction, checking its original bytes,
planning the edit and verifying the result.

The locations come from Core's [UnlockFrameRate patch](../../patches/unlock_frame_rate/patch.cpp). Core supports
a configurable limit and an unlimited setting. This example uses a fixed 120 FPS limit so the change fits
in the original instruction. It shows the whole process with one small edit.

Use an x86 SDK and an x86 MSVC developer shell, with the tools listed in
[build requirements](getting_started.md#build-requirements). You also need one of the executable builds below.
The plugin changes memory in the running process; it leaves the executable file unchanged.

## Identify the code to change

FC recognizes a build through its image profile. That tells a patch which executable it is working with.
The patch author still needs to establish which instructions implement the behavior and where they are
in each build.

In these builds, the game's frame timing code divides by a 64-bit frame rate value. It passes that value
as two 32-bit words on the stack: a high word of zero, then a low word of 80. These adjacent x86 instructions
set up those words:

```asm
push 0       ; 6A 00: high word
push 80      ; 6A 50: low word, the frame rate limit
```

Replacing `6A 50` with `6A 78` changes the low word to 120. The `6A` instruction encodes a signed one-byte
constant and extends it to the stack word's size. Both 80 and 120 fit. A value such as 144 would become
negative with this encoding, which is why Core's configurable patch uses a different instruction sequence.

These are the locations used by Core:

| Build | Profile ID | Frame timing function RVA | High-word instruction RVA | Limit instruction RVA |
| --- | --- | --- | --- | --- |
| Steam | `BF2.Steam.59EDE353` | `0x13AB20` | `0x13AB79` | `0x13AB7B` |
| GOG | `BF2.GOG.59EDF52B` | `0x13B880` | `0x13B8D9` | `0x13B8DB` |

An **RVA** is an offset from the start of the loaded executable. For example, if the executable loads at
`0x400000`, the Steam limit instruction is at `0x53AB7B`. FC adds the loaded base address itself. The
patch supplies `0x13AB7B`, not that absolute address or an offset into the file on disk.

When developing a new patch, inspect each target binary in a disassembler such as Ghidra to establish the
function's behavior, instruction boundaries and relevant values. Searching for `6A 50` alone is insufficient:
many unrelated instructions can push 80. Here the existing Core implementation supplies the identified
locations and their purpose.

The patch checks both words. The write requires the original limit instruction to be `push 80`, and a
separate check requires the high word to remain zero. These checks cover the assumptions behind this edit.

## Write the plugin

Create a directory containing `CMakeLists.txt` and `plugin.cpp`. The complete plugin is named `FrameRateExample`;
it contains the patch `FrameRateExample.FrameRate`.

### CMakeLists.txt

```cmake
cmake_minimum_required(VERSION 3.28)
project(FrameRateExample LANGUAGES CXX)

find_package(FusionCutter CONFIG REQUIRED)

add_library(FrameRateExample MODULE plugin.cpp)
target_link_libraries(FrameRateExample PRIVATE FusionCutter::SDK)
```

### plugin.cpp

```cpp
#include <fc/native.hpp>
#include <fc/plugin.hpp>
#include <cstdint>

namespace frame_rate_example {

class Handler {
  public:
    // Checks the frame divisor and plans a replacement for its low word.
    void plan(fc::Plan &plan) {
        const std::uint32_t update_time = plan.profile() == "BF2.Steam.59EDE353" ? 0x13ab20 : 0x13b880;

        // The game passes a 64-bit divisor; its high word must still be zero.
        plan.check(plan.at(update_time + 0x59, "Frame divisor high word"), 2, fc::exact_bytes({0x6a, 0x00}));

        const fc::CodeLocation limit{update_time + 0x5b, "Frame rate limit", fc::exact_bytes({0x6a, 0x50})};
        plan.write(limit, fc::byte_array<0x6a, 0x78>());
    }

    // Reports the limit installed by this patch.
    void write_status(fc::StatusContext &status) const noexcept {
        status.line("Frame rate limit: 120 FPS");
    }
};

// Selects this handler for the two verified client builds.
fc::Plugin build_plugin() {
    return {.id = "FrameRateExample",
            .version = "1",
            .patches = {fc::Patch{.id = "FrameRate",
                                  .description = "Set the game's frame rate limit to 120 FPS.",
                                  .enabled = true,
                                  .variants = {fc::variant<Handler>({"BF2.Steam.59EDE353", "BF2.GOG.59EDF52B"}, fc::Role::Client)}}}};
}

} // namespace frame_rate_example

FC_PLUGIN(frame_rate_example::build_plugin)
```

## How the plan works

The variant selects only the two listed profiles, in Client role. Dedicated server launches and other
builds do not select this handler. Because both builds use the same instructions at the same offsets within
the function, one handler supports both; only the function's RVA changes.

`plan.profile()` returns the selected profile ID. The conditional chooses Steam's RVA or GOG's RVA. Its
second branch means GOG because those are the only profiles admitted by this variant. When extending the
declaration, update this choice as well.

`plan.at` constructs a location in the selected image. The name, `"Frame divisor high word"`, identifies
that location in diagnostics. `plan.check` records the two bytes the patch expects to find there. A mismatch
fails the plan automatically; the handler does not need to repeat that error handling.

`fc::CodeLocation` combines the limit instruction's RVA, diagnostic name and expected bytes. `plan.write`
copies the replacement bytes into the plan. It leaves the running game unchanged during the Plan call.
The local `limit` and temporary byte array can disappear when the method returns.

The runtime validates the plan, checks for conflicting edits and rechecks the original bytes before
installation. It then writes the two replacement bytes. This patch needs no retained resources or
callbacks in game code, so Plan is its only installation method. `write_status` adds a line when FC collects
the installed patch's status.

`.enabled = true` makes the patch enabled by default. The default `configurable = true` also gives players
an INI toggle. The factory declares the plugin, and `FC_PLUGIN` exports it for the runtime to load.

## Build the DLL

From the example directory in the x86 developer shell:

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_PREFIX_PATH="C:/SDK/FusionCutter"
cmake --build build
```

Replace `C:/SDK/FusionCutter` with your installed x86 SDK. The output is `build/FrameRateExample.dll`.
Keep its PDB with the matching DLL for debugging.

## Verify both builds without launching the game

The offline verifier runs this plugin's declarations and Plan against supplied game files. It can catch
a wrong RVA, different original bytes or conflicting edits before you launch the game.

Core's `UnlockFrameRate` edits the same instruction. Disable it for this example, both in the verifier's
configuration and later in the game's configuration. Create a `verify-config` directory beside the example's
`CMakeLists.txt`, containing this `FC.Core.ini`:

```ini
[Client]
UnlockFrameRate = false
```

Use the verifier from the x86 SDK. Run it separately for each build, replacing the SDK and executable paths
with your own:

```powershell
& "C:/SDK/FusionCutter/tools/FusionCutterVerify.exe" --image "BF2.Steam.59EDE353=C:/Games/BF2-Steam/GameData/BattlefrontII.exe" --plugin "./build/FrameRateExample.dll" --config "./verify-config" --check FrameRateExample.FrameRate --json "./steam-verification.json"
& "C:/SDK/FusionCutter/tools/FusionCutterVerify.exe" --image "BF2.GOG.59EDF52B=C:/Games/BF2-GOG/GameData/BattlefrontII.exe" --plugin "./build/FrameRateExample.dll" --config "./verify-config" --check FrameRateExample.FrameRate --json "./gog-verification.json"
```

`--check FrameRateExample.FrameRate` requires this patch to be selected, enabled and validated. Without
that argument, a report with no failed checks could simply mean that your patch was never selected.

Look for a passed result for `FrameRateExample.FrameRate` and inspect any failed or unchecked items in the
report. The two checks establish that the plan fits both supplied files. They do not measure frame rate or
run the patched instructions. See [verification](verification.md) for the verifier's full scope.

## Install and check the result

1. Close the game. Install Fusion Cutter using the [Steam/GOG loader instructions](../installation.md#original-games-and-mod-tools).
2. In the game's existing `FC.Core.ini`, set `UnlockFrameRate = false` under `[Client]`. Keep its other entries.
   If the file does not exist yet, start and close the game once to generate it.
3. Copy `build/FrameRateExample.dll` into the game's `plugins` directory beside `FusionCutter.dll`.
4. Start the game and inspect `FusionCutter.txt`. Confirm that `FrameRateExample.FrameRate` is installed and
   that **Patch Status** contains `Frame rate limit: 120 FPS`.
5. Use a frame rate counter in a scene where the game previously reached its 80 FPS limit. The new limit
   should allow it to exceed 80, up to 120 FPS. VSync, driver limits and rendering performance can impose a
   lower limit.

The runtime creates `FC.FrameRateExample.ini` with:

```ini
[FrameRate]
Enabled = true
```

Set `Enabled = false` and restart to compare with the original limit, keeping Core's `UnlockFrameRate`
disabled for that comparison. Re-enable the example and restart to check the change again.

If installation fails, start with **Issues** in `FusionCutter.txt`. An incompatible profile means the
executable does not match this variant. An evidence mismatch means the code differs from the expected
bytes; inspect the reported address before changing the evidence. A conflict at the limit instruction
can mean Core's `UnlockFrameRate` or another patch is still enabled. Use
[Debug logging](../troubleshooting.md#diagnostic-files) when more detail is needed.

To return to Core's configurable limit, close the game, remove `FrameRateExample.dll` and re-enable
`UnlockFrameRate` in `FC.Core.ini`.

## Support another build

Adding an ID to `.variants` is only the selection change. To support another build:

1. Confirm that FC recognizes it and obtain its exact profile ID.
2. Locate its frame timing code and determine whether it uses the same divisor setup and instruction encoding.
3. Add its locations to the handler's build selection. If its instruction sequence differs, describe the
   operations it actually needs rather than reusing these offsets.
4. Add the profile to the variant, run the verifier against that executable and check the behavior in the game.

Use a separate handler and variant when the implementation differs enough to make that clearer. A different
RVA alone does not require one. A wildcard would accept every matching recognized profile, so use explicit
IDs until the patch has layouts and evidence for the builds it would select.

The [memory guide](native_memory.md) expands the location, evidence and write APIs used here.
The [hook guide](native_hooks.md) covers changes that need to run C++ code when the game calls a function or
reaches an instruction.
