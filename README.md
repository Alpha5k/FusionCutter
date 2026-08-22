# FusionCutter

FusionCutter is a modular runtime patching framework for *Star Wars Battlefront II* (2005). It lets fixes and new
features ship as independent plugin DLLs instead of permanent edits to game executables.

At startup, FusionCutter recognizes the exact game build, reads the user's configuration, checks each patch's
assumptions, detects conflicts, and applies the selected changes in memory. The project aims to give patch authors one
shared foundation for code and data changes, hooks, settings, runtime behavior, diagnostics, and validation across
supported clients and dedicated servers.

FusionCutter is still early in development. Release packaging and end-user tooling are incomplete, and the default
build does not yet include gameplay patches.

## What FusionCutter provides

- Independent plugins that can each contribute one or more patches for users to enable and configure.
- Exact recognition of reviewed game builds instead of guessing from a filename or similar executable.
- Coordinated writes, redirects, hooks, and native data allocation with evidence checks and conflict detection.
- Typed settings, patch relationships, shared hooks, patch interfaces, and support for persistent runtime behavior.
- Shared logs, current status, crash snapshots, and bounded high-volume tracing.
- A public C++ SDK, non-mutating plugin testing API, and game image verifier.

Patches modify only the loaded process. FusionCutter does not rewrite the original game executable or DLL on disk.

## Current target support

FusionCutter currently recognizes exact reviewed images for:

| Game family | Architecture | Roles and images |
| --- | --- | --- |
| Steam retail | x86 | Client or server; main game executable |
| GOG retail | x86 | Client or server; main game executable, plus the server's optional Galaxy peer library |
| Mod Tools | x86 | Client; main Mod Tools executable |
| Classic Collection | x64 | Client bootstrap and game DLL, or server game DLL |

Each patch declares its own supported targets. Recognition of a game build does not mean that every plugin or patch
supports it. Only exact reviewed images are accepted; GameSpy executable profiles are not yet available.

## Installation

There is no finished release package yet. Build the required architecture, then use the files under
`build/<preset>/artifacts/<configuration>/`. Place external plugin DLLs in a `plugins` directory beside
`FusionCutter.dll`.

### Steam, GOG, or Mod Tools client

Copy these x86 files beside the game or Mod Tools executable:

- `dinput8.dll`
- `FusionCutter.dll`

Launch the game normally. If another DirectInput proxy already uses `dinput8.dll`, keep FusionCutter as `dinput8.dll`
and rename the other proxy to one matching `dinput8_*.dll`, such as `dinput8_reshade.dll`. FusionCutter chains exactly
one matching proxy. If several files match, none of them is selected.

### Classic Collection client

In the Classic Collection installation directory:

1. Rename the original `Battlefront2.dll` to `Battlefront2.original.dll`.
2. Copy the x64 FusionCutter `Battlefront2.dll` and `FusionCutter.dll` beside it.
3. Launch the game normally through `Battlefront.exe`.

To remove the loader, delete FusionCutter's `Battlefront2.dll` and rename `Battlefront2.original.dll` back to
`Battlefront2.dll`.

### Dedicated server

The server loader uses the existing SWBF2Admin RconServer injection path. Place `FusionCutter.dll` beside the matching
loader in the configured server directory:

- Steam or GOG server: x86 `RconServer_32.dll` and x86 `FusionCutter.dll`
- Classic Collection server: x64 `RconServer_64.dll` and x64 `FusionCutter.dll`

Keep the matching SWBF2Admin `DllLoader` executable in that directory and enable runtime management as usual.

## Configuration and diagnostics

FusionCutter creates configuration and diagnostic files beside `FusionCutter.dll`:

| Path | Purpose |
| --- | --- |
| `config/FC.Core.ini` | Framework logging and trace settings |
| `config/FC.<PluginId>.ini` | Enable switches and settings for one admitted plugin |
| `FusionCutter.txt` | Current initialization, target, plugin, patch, and trace status |
| `FusionCutter.log` | Detailed diagnostics, created when the configured log level accepts a record |
| `FusionCutter.Crash.log` | Bounded crash snapshot written when FusionCutter captures a native failure |
| `traces/FusionCutter-*.etl` | High-volume plugin traces, created when an installed patch records trace data |

Framework settings are always generated. A plugin configuration is generated only when an applicable patch or group
contributes an enable switch or settings. Check `FusionCutter.txt` first when startup or a patch does not behave as
expected, then use `FusionCutter.log` for details.

## Building

Requirements:

- Windows
- Visual Studio 2022 with the MSVC v143 x86 and x64 toolsets and a Windows SDK
- CMake 3.28 or newer
- Git
- clang-format 22 when contributing C or C++ changes

Clone the repository with its pinned submodules:

```powershell
git clone --recurse-submodules https://github.com/Alpha5k/FusionCutter.git
cd FusionCutter
```

For an existing checkout, initialize or update the submodules before configuring:

```powershell
git submodule update --init --recursive
```

Configure and build either architecture with the checked-in presets:

```powershell
cmake --preset vs2022-x86
cmake --build --preset vs2022-x86 --config RelWithDebInfo

cmake --preset vs2022-x64
cmake --build --preset vs2022-x64 --config RelWithDebInfo
```

The primary artifacts are:

| Build | Artifacts |
| --- | --- |
| x86 | `FusionCutter.dll`, `dinput8.dll`, `RconServer_32.dll`, `FusionCutterVerify.exe` |
| x64 | `FusionCutter.dll`, `Battlefront2.dll`, `RconServer_64.dll`, `FusionCutterVerify.exe` |

Tests are enabled by default and run through CTest after the corresponding build completes:

```powershell
ctest --test-dir build/vs2022-x86 -C RelWithDebInfo --output-on-failure
ctest --test-dir build/vs2022-x64 -C RelWithDebInfo --output-on-failure
```

## Plugin authoring

Plugins are ordinary architecture-specific DLLs built against an installed FusionCutter SDK. A plugin may contain a
single checked edit or several configurable patches with hooks, runtime state, relationships, and shared services.

- [Getting Started](docs/GETTING_STARTED.md) builds and explains a first plugin.
- [SDK Reference](docs/SDK_REFERENCE.md) documents the complete public authoring and testing API.
- [Plugin Best Practices](docs/BEST_PRACTICES.md) explains how to build patches around framework-owned facilities.

Install a matching SDK from a local build with:

```powershell
cmake --install build/vs2022-x86 `
    --config RelWithDebInfo `
    --prefix C:\path\to\FusionCutterSDK-x86
```

Use the x64 build directory and a separate prefix for x64 plugins.

`FusionCutterVerify.exe` recognizes real game images and validates plugins through the same non-mutating path used by
the public testing API:

```powershell
build/vs2022-x86/artifacts/RelWithDebInfo/FusionCutterVerify.exe `
    --role client `
    --image "Game=C:\path\to\BattlefrontII.exe" `
    --plugin "C:\path\to\ExamplePatches.dll" `
    --check ExampleFix
```

The verifier does not launch the game or modify the supplied image. Keep proprietary game files outside the
repository.

## Contributing

Before changing the framework, read:

- [CONTRIBUTING.md](CONTRIBUTING.md) for setup, build, test, formatting, and validation workflows;
- [STYLE_GUIDE.md](docs/STYLE_GUIDE.md) for source style, naming, organization, and comments; and
- [DESIGN_PRINCIPLES.md](docs/DESIGN_PRINCIPLES.md) for project priorities and framework-wide principles.

## License

FusionCutter is licensed under the [MIT License](LICENSE). Vendored dependencies retain their own licenses.
