# Fusion Cutter

Fusion Cutter is a runtime patching framework for Star Wars Battlefront (2004), Star Wars Battlefront II
(2005) and Star Wars: Battlefront Classic Collection. It provides a framework for writing patches for the game and
publishing them as plugins that can fix bugs, add new features, or modify game behavior.

The goal of Fusion Cutter is to make it as easy and safe as possible to write patches for the game without worrying
about compatibility with different game builds, conflicts with other mods and plugins, or how to hook into the game safely.
Game modifications can be shipped as plugins that get loaded, applied, and validated by Fusion Cutter, while the runtime
handles configuration, dependencies, logging, crash reporting, and shared services.

Fusion Cutter provides loaders for all game versions including dedicated servers, a runtime, and a C++ SDK. It supports a
number of features beyond simple memory edits and hooks including session identification, multiplayer messaging, lobby
publishing, and advanced diagnostics.

Note: This project is still in its early stages. While the ABI is not expected to change at this point, compatibility
between FusionCutter versions is not guaranteed until 1.0 release.

## Features

A plugin contains one or more **patches**, each implementing a feature. Patches declare the game builds and
launch roles they support. The runtime selects the matching implementation, reads its settings, resolves its
dependencies and installs its changes.

This gives plugins a common way to:

- **Modify game behavior.** Read and write game memory, install function or instruction hooks, call
  game functions and allocate resources through the runtime.
- **Share hooks between patches.** Compatible observers and one replacement callback can use the same
  game location, even from separate plugins. The runtime manages [shared hooks](docs/sdk/native_hooks.md#shared-hooks).
- **Check compatibility before making changes.** Match known executables and DLLs, check expected code or
  data at patch locations, and detect conflicting edits.
- **Use the same configuration system.** Declare typed settings, defaults and validation, then let the runtime
  generate the applicable INI entries.
- **Respond to the game session.** Follow session changes, track multiplayer peers, exchange custom messages and
  publish or retrieve lobby metadata on supported builds.
- **Extend the game's Lua scripts.** Register C++ callbacks and global values, or exchange messages between
  scripts on different peers.
- **Share functionality between patches.** Publish APIs that other patches can use to build on an existing feature.
- **Diagnose problems.** Write logs and status information, and collect detailed binary traces.

The loaders start Fusion Cutter inside the game. The runtime manages patch installation and shared services;
the SDK supplies the interfaces authors use to build plugins. Plugins execute in the game's
address space, so compatibility checks cannot prevent arbitrary errors in plugin code.

## Supported games

The [build catalog](docs/installation.md#recognized-builds) lists the exact versions covered by each column.
**Client**, **Server** and **Both** identify the supported launch roles; **—** means unsupported.
Match the loader, runtime and plugins to the game's architecture, even on 64-bit Windows.

### Battlefront I

| Feature | Steam | DVD | SPTest | Classic Collection |
| --- | --- | --- | --- | --- |
| Architecture | x86 | x86 | x86 | x64 |
| Runtime and plugin loading | Both | Both | Both | Both |
| Lua bindings | — | — | — | — |
| Lua multiplayer | — | — | — | — |
| Game context | — | — | — | — |
| Multiplayer messaging | — | — | — | — |
| Lobby publishing | — | — | — | — |

### Battlefront II

| Feature | Steam | GOG | DVD | ModTools | Classic Collection |
| --- | --- | --- | --- | --- | --- |
| Architecture | x86 | x86 | x86 | x86 | x64 |
| Runtime and plugin loading | Both | Both | Both | Both | Both |
| Lua bindings | Both | Both | — | Both | — |
| Lua multiplayer | Both | Both | — | — | — |
| Game context | Both | Both | — | Both (local only) | — |
| Multiplayer messaging | Both | Both | — | — | — |
| Lobby publishing | Both | Both | — | — | — |

Game context detects only local missions in ModTools. Multiplayer messaging, Lua multiplayer and lobby
publishing require Galaxy. External plugins declare their own supported builds.

Lua bindings let C++ plugins register callbacks and values in the game's Lua states. Lua multiplayer
lets scripts exchange messages through the runtime's multiplayer service. Both provide facilities for
other mods; installing them alone does not add a new gameplay feature.

## Included patches

The **Core plugin** is built into the runtime; it does not need a separate plugin DLL. It includes the Lua
facilities above and these Battlefront II patches:

| Patch | What it does |
| --- | --- |
| Colored chats | Sets separate colors for normal, team, admin and buddy messages |
| Frame rate limit | Sets the game's maximum frame rate, or removes its limit with a value of `0`; defaults to 120 FPS |
| Dedicated server fixes | Corrects texture handling with `/norender`, password handling and lobby advertisement |
| Remote administration | Supports authenticated server commands, including Lua execution, from administration tools |

| Patch | Steam | GOG | DVD | ModTools | Classic Collection |
| --- | --- | --- | --- | --- | --- |
| Colored chats | Client | Client | — | — | — |
| Frame rate limit | Client | Client | — | Client | — |
| Dedicated server fixes | Server | Server | — | — | — |
| Remote administration | — | Server | — | — | Server |

The Client or Server role is selected by the loader. Hosting a match from a client launch keeps the Client
role.

Additional plugins are distributed separately. Put their DLLs directly in the `plugins` directory beside the
runtime, and follow their instructions for any scripts or other assets.

## Getting started

Use matching loader and runtime files from the same Fusion Cutter build. To produce them from this repository,
see [building from source](#building-from-source).

For an original Battlefront II client, place the x86 files beside the game executable:

```text
GameData/
    BattlefrontII.exe
    dinput8.dll
    FusionCutter.dll
    plugins/
        ExamplePlugin.dll
```

`plugins/ExamplePlugin.dll` illustrates where an optional plugin goes. The Core plugin works without that
directory or any external plugin.

1. Close the game and install the loader and runtime using the [installation guide](docs/installation.md).
   If another mod already supplies `dinput8.dll`, read the guide's chaining instructions before replacing it.
2. Start the game. Fusion Cutter writes `FusionCutter.txt` beside the runtime with the recognized build,
   loaded plugins, patch outcomes and any reported issues.
3. Close the game before changing configuration or plugin files. Restart to apply the changes.

Other installations use these loaders:

| Installation | Loader |
| --- | --- |
| Original Battlefront I and supported mod tools | x86 `dinput8.dll`, beside the relevant executable |
| Classic Collection clients | x64 `Battlefront1.dll` or `Battlefront2.dll` proxy; preserve the game's DLL as the corresponding `.original.dll` |
| Dedicated servers | `RconServer_32.dll` for original games, or `RconServer_64.dll` for Classic Collection |

Client loaders need an adjacent `FusionCutter.dll`. Each dedicated loader contains the runtime and must be
loaded by a server launcher or injector; copying it into the directory alone does not start it. Use one
loader route per process. The [installation guide](docs/installation.md) covers each layout, updates and removal.

## Configuration

Fusion Cutter creates configuration beside the runtime:

- `FusionCutter.ini` controls runtime logging and tracing.
- `FC.Core.ini` contains applicable settings for the bundled patches.
- `FC.<PluginId>.ini` contains settings for an external plugin.

Only patches compatible with the current executable and launch role contribute settings. A plugin with no
applicable settings does not produce an empty INI file.

For example, these entries in `FC.Core.ini` enable the two client patches and set a 144 FPS limit:

```ini
[Client]
ColoredChats = true
UnlockFrameRate = true

[UnlockFrameRate]
MaxFrameRate = 144
```

Edit the generated entries rather than adding duplicate keys or sections. Changes take effect after a
restart. See [configuration](docs/configuration.md) for the complete rules, defaults and error handling.

## Writing plugins

A plugin is a DLL built with the C++23 SDK. It declares its patches, settings, dependencies and supported
builds. Each selected patch has a handler that implements the stages it needs, such as planning memory edits
or registering callbacks. Variants let a patch supply different layouts or implementations for different
game builds.

Authors describe memory edits and hooks in a **plan** before the runtime applies them. A plan records the target
locations, expected code or data, and requested edits, hooks or resources. The runtime validates these
requests and manages installation. Patches that only use existing services can work without modifying game memory.

Start with [how plugins work](docs/sdk/concepts.md), then follow [your first plugin](docs/sdk/getting_started.md).
The tutorial builds a plugin with a visible status entry and configuration, without needing game addresses.
Continue with [your first game patch](docs/sdk/game_patch.md) to change the frame rate limit on Steam and GOG.
The maintained [example project](sdk/example) and [public headers](sdk/include/fc) are included in this repository.

The [SDK documentation](docs/sdk/README.md) combines explanations, code examples and API references for memory
edits and hooks, settings, lifecycle, patch APIs, game context, multiplayer, lobby, Lua and reporting. Players
need the resulting plugin DLL and its required assets; the SDK and compiler are only needed to build it.

## Troubleshooting and verification

Start with `FusionCutter.txt` for the current launch. `FusionCutter.log` records events at the configured log
level, and `FusionCutter.crash.log` contains exception or fatal-error reports when available. Patches can also
submit detailed events to one ETW trace file per session under `traces/`. ETW is Event Tracing for Windows,
and these binary files are intended for diagnostic analysis.

The default log level is `Warning`. Set `LogLevel = "Debug"` in the existing `[FusionCutter]` section of
`FusionCutter.ini` when more detail is needed, then restart and reproduce the issue.

`FusionCutterVerify.exe` checks game recognition and patch plans against supplied game files without launching
the game. It helps identify incompatible builds and invalid patch plans before installation; gameplay,
installed hooks and multiplayer behavior still need testing in the game.

See [troubleshooting](docs/troubleshooting.md) for interpreting patch outcomes, running the verifier and
collecting a useful problem report.

## Building from source

Build on Windows with a C++23-capable MSVC toolset, the Windows SDK, CMake 3.28 or later, Ninja and Git.
Use an MSVC developer shell configured for the target architecture.

From the repository root, build and test the x86 binaries for the original games:

```powershell
git submodule update --init --recursive
cmake --preset x86-release
cmake --build --preset x86-release --parallel
ctest --preset x86-release
```

Use `x64-release` in an x64 developer shell for Classic Collection. Both release presets use `RelWithDebInfo`,
which includes debug symbols. Build output goes to `../build/<preset>` outside the source tree.

To collect the runtime files or SDK into an installation directory:

```powershell
cmake --install ../build/x86-release --prefix ../build/stage/x86-release --component Runtime
cmake --install ../build/x86-release --prefix ../build/stage/x86-release --component SDK
```

See [CONTRIBUTING.md](CONTRIBUTING.md) for component builds, other presets and testing guidance.
Follow [STYLE.md](STYLE.md) for code and writing conventions.

## Documentation and source

| Guide | Read it for |
| --- | --- |
| [Installation](docs/installation.md) | Loader selection, exact supported builds, file layouts, updates and removal |
| [Configuration](docs/configuration.md) | INI settings, patch and group controls, defaults and validation |
| [Architecture](docs/architecture.md) | Source layout, startup, patch installation, threads and resource lifetime |
| [Troubleshooting](docs/troubleshooting.md) | Status, logs, traces, failures and problem reports |
| [SDK documentation](docs/sdk/README.md) | Plugin authoring guides, examples and API references |

The main components are in [loaders](loaders), [runtime](runtime) and [sdk](sdk). The embedded plugin is declared
in [plugins](plugins), with its implementations in [patches](patches). The verifier is in [tools](tools).

## License

Fusion Cutter uses the [MIT license](LICENSE). Distribution packages include notices and licenses for their
third-party dependencies.
