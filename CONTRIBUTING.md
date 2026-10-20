# Contributing

See [STYLE.md](STYLE.md) for code and writing conventions.

## Building

Development requires Windows, a C++23-capable MSVC toolset, the Windows SDK, CMake 3.28 or later, Ninja and Git.
Use an MSVC developer shell for the target architecture, with CMake and Ninja on `PATH`.
Run commands from the repository root. Initialize the dependencies before configuring:

```powershell
git submodule update --init --recursive
```

Choose the preset matching your developer shell:

| Preset | Architecture | Configuration |
| --- | --- | --- |
| `x86-debug` | x86 | Debug |
| `x64-debug` | x64 | Debug |
| `x86-release` | x86 | RelWithDebInfo |
| `x64-release` | x64 | RelWithDebInfo |

For example, configure, build and test x64 with debug symbols:

```powershell
cmake --preset x64-release
cmake --build --preset x64-release --parallel
ctest --preset x64-release
```

Build output goes to `../build/<preset>`. Both release presets use `RelWithDebInfo`.

Components can also be configured directly with `cmake -S sdk -B ../build/sdk`, replacing `sdk` with
`runtime`, `loaders`, `patches`, `plugins` or `tools` as needed. These use the same targets as the root build.

`FC_VERSION_STRING` in `cmake/project.cmake` sets the shared product version.

## Testing changes

Run the tests for the affected components. Use `ctest --preset <preset> -R <expression>` to select cases by name.
Changes to the ABI, calling conventions, game memory layouts, hooks or resource lifetimes need testing on
x86 and x64 in both Debug and RelWithDebInfo.

Check assumptions about game code and data against the supported binaries. For relocatable images, validate
patch plans at another load address: embedded addresses must use the selected module's base and RVA, and
byte checks must account for relocations.

Add focused tests for changed behavior and update affected documentation. When submitting a change, include
the relevant test results and game builds checked, distinguishing automated checks, static analysis and live testing.

## Staging an installation

Collect the runtime or SDK into a local installation directory:

```powershell
cmake --install ../build/x64-release --prefix ../build/stage/x64-release --component Runtime
cmake --install ../build/x64-release --prefix ../build/stage/x64-release --component SDK
```
