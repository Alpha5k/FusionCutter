# Contributing to FusionCutter

This guide covers the development workflow for FusionCutter: setting up the repository, building the project, running
tests, and validating a change. See [STYLE_GUIDE.md](docs/STYLE_GUIDE.md) for source formatting, naming, and comments,
and [DESIGN_PRINCIPLES.md](docs/DESIGN_PRINCIPLES.md) for project-wide design guidance.

## Requirements

- Windows
- Visual Studio 2022 with the MSVC v143 x86 and x64 toolsets and a Windows SDK
- CMake 3.28 or newer
- Git
- clang-format 22 for C or C++ changes

FusionCutter uses C++23. The checked-in CMake presets provide the supported compiler, architecture, C++ runtime, and
warning configuration.

## Set up the repository

Clone the repository with its pinned submodules:

```powershell
git clone --recurse-submodules https://github.com/Alpha5k/FusionCutter.git
cd FusionCutter
```

For an existing checkout, initialize or update the submodules before configuring:

```powershell
git submodule update --init --recursive
```

Vendored dependencies are kept under `vendor/` and must remain usable offline. Do not modify or replace them as part of
an unrelated change.

## Build the project

FusionCutter provides separate Visual Studio presets for each supported architecture:

| Architecture | Preset |
| --- | --- |
| x86 | `vs2022-x86` |
| x64 | `vs2022-x64` |

Configure and build the architecture needed by your change:

```powershell
cmake --preset vs2022-x86 -DBUILD_TESTING=ON
cmake --build --preset vs2022-x86 --config Debug
```

`BUILD_TESTING` controls whether the test targets are configured. Use `vs2022-x64` for the x64 build. `Debug` is
intended for development; use `RelWithDebInfo` for an optimized build with symbols:

```powershell
cmake --build --preset vs2022-x86 --config RelWithDebInfo
```

Build one target while iterating with:

```powershell
cmake --build --preset vs2022-x86 --config Debug --target <target>
```

Changes to shared code, the public SDK, or the C ABI should be built for both architectures. Build output remains under
the selected preset's `build/` directory; project targets must not write directly into a game installation.

## Install and test a local build

For runtime testing, copy the matching `FusionCutter.dll`, loader, and any external plugin DLLs from the
architecture-specific build output into a separate test installation. External plugins belong in the `plugins`
directory beside `FusionCutter.dll`.

| Target | Loader |
| --- | --- |
| GameSpy, Steam, GOG, or Mod Tools client | x86 `dinput8.dll` |
| GameSpy, Steam, or GOG server | x86 `RconServer_32.dll` |
| Classic Collection client | x64 `Battlefront2.dll` |
| Classic Collection server | x64 `RconServer_64.dll` |

For a Classic Collection client, preserve the original `Battlefront2.dll` as `Battlefront2.original.dll` before
installing the proxy. Keep generated configuration, status, logs, traces, and proprietary game files out of the
repository.

## Run the tests

Project tests use Catch2 and are run through CTest. CTest runs tests that have already been built; its `-C` value must
match the configuration passed to `cmake --build`.

| Label | Intended use |
| --- | --- |
| `fast` | Unit and component tests run during ordinary development |
| `integration` | Tests across DLL, loader, native, plugin, runtime, and process boundaries |
| `images` | Target recognition and validation of supported images using local reviewed binaries |

Run the fast suite for the affected architecture during normal development:

```powershell
ctest --test-dir build/vs2022-x86 -C Debug -L fast --output-on-failure
```

Run one matching test or component while iterating with:

```powershell
ctest --test-dir build/vs2022-x86 -C Debug -R <pattern> --output-on-failure
```

Run integration tests when a change affects DLL loading, plugins, loaders, native memory, patch installation, runtime
behavior, or another process boundary:

```powershell
ctest --test-dir build/vs2022-x86 -C Debug -L integration --output-on-failure
```

Run all tests registered for a build with:

```powershell
ctest --test-dir build/vs2022-x86 -C Debug --output-on-failure
```

Replace the preset directory with `build/vs2022-x64` when testing x64. Changes to shared code, the SDK, or the C ABI
should run the relevant suites for both architectures.

Tests labeled `images` require locally configured, reviewed game binaries. These binaries must never be committed.
Image tests are normally required only when changing target recognition, native locations, evidence, or the behavior
of supported images.

### Adding tests

Add or update a test when a change affects consequential behavior, a stable public boundary, a native or process
boundary, or a defect likely to recur. Prefer extending an existing table, fixture, or end-to-end slice over creating a
new test executable or fixture for each variation. Tests should exercise production paths rather than reproduce their
logic in test-only code.

## Format source code

Format project-owned C and C++ and then verify the result:

```powershell
./tools/format.ps1
./tools/format.ps1 -Check
```

The script requires clang-format major version 22. Formatting and other source conventions are defined in
[STYLE_GUIDE.md](docs/STYLE_GUIDE.md).

## Before handing off a change

- Keep the change focused and preserve unrelated work.
- Run the formatting check for C or C++ changes.
- Build every affected architecture and configuration.
- Run the smallest relevant tests, plus any affected integration or image tests.
- Update documentation when behavior, public interfaces, or contributor workflow changes.
- State any validation that could not be performed.
