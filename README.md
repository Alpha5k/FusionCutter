# Fusion Cutter

Fusion Cutter is a source-level client and server patch framework for *Star Wars Battlefront II* (2005). It loads one shared patch core through small, target-specific loaders and applies compiled patches only to reviewed executable layouts.

The project currently supports the Steam and current GOG retail releases, Mod Tools, and the Classic Collection. Available patches are selected by target and role, so one codebase can provide client fixes, dedicated-server support, configurable engine limits, and multiplayer improvements without modifying the game files on disk.

## Patch development

- [Writing a Patch](docs/patches.md)
- [Patch Author Reference](docs/reference.md)

Fusion Cutter is under active development. Release packaging and end-user documentation are not yet complete; the instructions below describe the current development artifacts.

## Installation

Build the architecture and configuration required by your game, then find the files under `build/<preset>/artifacts/<configuration>/`.

### Steam, GOG, or Mod Tools client

Copy these x86 artifacts beside the executable in `GameData`:

- `dinput8.dll`
- `FusionCutter.dll`

Launch the game normally. If another DirectInput proxy such as ReShade already owns `dinput8.dll`, keep Fusion Cutter as `dinput8.dll` and rename the other proxy to one matching `dinput8_*.dll`, such as `dinput8_reshade.dll`. Fusion Cutter chains one matching proxy; multiple matches disable chaining.

### Classic Collection client

In the Classic Collection installation directory:

1. Rename the original `Battlefront2.dll` to `Battlefront2.original.dll`.
2. Copy the x64 Fusion Cutter `Battlefront2.dll` and `FusionCutter.dll` beside it.
3. Launch the game normally through `Battlefront.exe`.

To remove the loader, delete Fusion Cutter's `Battlefront2.dll` and rename `Battlefront2.original.dll` back to `Battlefront2.dll`.

### Dedicated server

SWBF2Admin loads the server through the existing RconServer injection path. Place `FusionCutter.dll` beside the matching loader in the configured server directory:

- GOG server: x86 `RconServer_32.dll` and x86 `FusionCutter.dll`
- Classic Collection server: x64 `RconServer_64.dll` and x64 `FusionCutter.dll`

Keep the matching SWBF2Admin `DllLoader` executable in that directory and enable runtime management as usual.

On first startup, Fusion Cutter generates only the configuration entries supported by the active target and role. Client files use `FusionCutter.*`; server files use `FusionCutter-Server.*`. Check `FusionCutter.txt` or `FusionCutter-Server.txt` for the initialization result and patch status.

## Building

Requirements:

- Windows
- Visual Studio 2022 with the MSVC v143 x86/x64 toolchain and a Windows SDK
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

The x86 build produces `FusionCutter.dll`, `dinput8.dll`, and `RconServer_32.dll`. The x64 build produces `FusionCutter.dll`, `Battlefront2.dll`, and `RconServer_64.dll`. Universal client/server cores are built by default.

Tests are enabled by default and run locally through CTest:

```powershell
ctest --test-dir build/vs2022-x86 -C RelWithDebInfo --output-on-failure
ctest --test-dir build/vs2022-x64 -C RelWithDebInfo --output-on-failure
```

See [CONTRIBUTING.md](CONTRIBUTING.md) for the project requirements, style, formatting, and validation rules.

## License

Fusion Cutter is licensed under the [MIT License](LICENSE). Vendored dependencies retain their own licenses.
