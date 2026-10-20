# Your first plugin

This example adds a configurable line to `FusionCutter.txt`. It exercises plugin export, build selection,
settings and status without modifying game memory.

You will build a DLL named `ExamplePlugin.dll` containing one patch, `ExamplePlugin.Status`. The player can
enable or disable the patch and choose the number it displays. Seeing that number in FC's status report
confirms that the runtime loaded the DLL, selected the variant and constructed its handler with settings.

If the relationship between a plugin, patch and handler is new, read [how plugins work](concepts.md) first.
This tutorial walks through the complete source; later guides expand each API used here.

## Build requirements

Use Windows, an MSVC toolset with C++23 support, CMake 3.28 or later and Ninja. Obtain an installed Fusion
Cutter SDK, or stage it from this repository using the SDK component in [CONTRIBUTING.md](../../CONTRIBUTING.md).
The package contains headers, a CMake target, the verifier and the [maintained example](../../sdk/example).

Build x86 plugins for the original games and x64 plugins for Classic Collection. Select the matching MSVC
developer shell. The same source can declare variants for both architectures; the runtime selects compatible
builds. Separate machine-code layouts belong in those variants.

Create these two files in your plugin project.

## CMakeLists.txt

```cmake
cmake_minimum_required(VERSION 3.28)
project(ExamplePlugin LANGUAGES CXX)

find_package(FusionCutter CONFIG REQUIRED)

add_library(ExamplePlugin MODULE plugin.cpp)
target_link_libraries(ExamplePlugin PRIVATE FusionCutter::SDK)
```

`FusionCutter::SDK` supplies include paths, C++23, Windows definitions and MSVC options, including the static
C runtime (`/MT`, or `/MTd` in Debug). It is an interface target; plugin calls reach the runtime through its
provided tables rather than an import library.

`MODULE` asks CMake to build a dynamically loaded DLL. The game will not call a `main` function in this
project. Instead, FC discovers the export supplied by `FC_PLUGIN` and uses it to read your declaration.

## plugin.cpp

```cpp
#include <fc/plugin.hpp>

namespace example {

struct Settings {
    int value = 7;
};

class Handler {
  public:
    using Settings = example::Settings;

    Handler(const fc::CreateContext &, const Settings &settings) : value_(settings.value) {}

    // Adds this patch's configured value to the runtime's status report.
    void write_status(fc::StatusContext &status) const noexcept {
        status.line("Example value: {}", value_);
    }

  private:
    int value_;
};

// Declares the example for every recognized Battlefront II image profile.
fc::Plugin build_plugin() {
    return {.id = "ExamplePlugin",
            .version = "1",
            .patches = {fc::Patch{
                .id = "Status",
                .enabled = true,
                .settings = fc::settings<Settings>({fc::setting("Value", &Settings::value, "Value displayed in status.").range(1, 20)}),
                .variants = {fc::variant<Handler>({"BF2.*"})}}}};
}

} // namespace example

FC_PLUGIN(example::build_plugin)
```

`FC_PLUGIN` appears once per DLL and exports the undecorated `FC_GetPlugin` name on both architectures.
Keep the factory limited to declarations; game access belongs in handler stages. The SDK retains the
declaration storage while the runtime uses it.

This broad selector is appropriate because the example only writes status. A patch that edits game code or
data needs verified layouts for every profile it selects.

## How the example works

`Settings` is an ordinary C++ object. Its `value = 7` initializer supplies the default. The `fc::setting`
call connects the public INI key `Value` to that member and accepts values from 1 through 20. You declare
this connection once; the runtime handles file generation and parsing, and the SDK constructs the typed object.

`Handler::Settings` tells the SDK which settings type the constructor expects. The constructor copies the
chosen value into `value_`. The handler keeps that copy because the constructor's settings argument is temporary.

`write_status` is a callback: FC calls it when collecting the current report. `status.line` contributes a
line under this patch's entry. The handler needs no Plan or Prepare methods because it makes no memory edits
and registers no other services.

`build_plugin` describes the plugin and its patch. The plugin ID determines the INI filename and the first
part of the qualified patch name; `Status` determines the second part. `.enabled = true` supplies the startup
default. Because `configurable` defaults to true, FC also creates an editable enabled toggle.

The variant connects `Handler` to recognized Battlefront II builds. The profile selector `BF2.*` selects
from FC's catalog; it does not add support for unknown executables. `FC_PLUGIN` makes the declaration available
to the runtime and retains its storage. None of the factory's declarations execute a status callback themselves.

## Configure, build and install

From the plugin project directory in the matching developer shell:

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_PREFIX_PATH="C:/SDK/FusionCutter"
cmake --build build
```

Replace `C:/SDK/FusionCutter` with the installed SDK directory. Copy `ExamplePlugin.dll` to the game's
`plugins` directory beside the runtime, then restart the game. See [installation](../installation.md) for
each loader's directory layout.

If your plugin needs other DLLs, see [plugin loading](abi.md#plugin-loading) for FC's search rules.

On an applicable build, `FC.ExamplePlugin.ini` contains:

```ini
[Status]
Enabled = true
Value = 7
```

The plugin appears in `FusionCutter.txt`, and **Patch Status** includes its value after installation.
Set `Value = 12`, restart and check for `Example value: 12`. Set `Enabled = false` and restart; the patch
should be disabled and contribute no custom status line. To check validation, try `Value = 30` with the
patch enabled. FC should report the invalid value and use the declared default, 7.

If the plugin does not appear, check its installation directory and DLL architecture first. If it appears
but the patch does not run, read **Issues** for selection, configuration or startup failures. Enable Debug
logging as described in [troubleshooting](../troubleshooting.md) when the normal report lacks enough detail.

For a plugin that only targets Battlefront I, replace the example's selector with suitable BF1 profiles.
The status behavior itself has no Battlefront II dependency; the declaration deliberately limits where it runs.

## Turn the example into a feature

Continue with [your first game patch](game_patch.md) for a complete frame rate patch using real Steam and GOG
locations. It covers the instruction change, build selection, evidence, offline verification and in-game checks.

Keep the plugin declaration and replace the handler's behavior. Add Plan when the feature edits game memory
or installs a hook. Add Prepare when it registers with game context, multiplayer, tracing or another patch's
API. Add Update only when it needs periodic work rather than an existing callback.

For instance, the [Lua example](lua.md#a-complete-binding-patch) exposes a callable function to game scripts
using a dependency and Prepare. The [patch API example](patch_apis.md#a-provider-and-required-consumer)
expands the status pattern into two cooperating patches. Both use the same export and build arrangement.

Keep the matching PDB with your build for diagnosis. Continue with [declarations](declarations.md) to add
categories, groups, dependencies and build-specific implementations.
