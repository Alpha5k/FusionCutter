# Installation

Choose the files for the game's architecture: **x86 (32-bit)** for the original Battlefront games and
**x64 (64-bit)** for Classic Collection. The operating system's architecture does not determine which build to use.

Close the game before installing or updating files. The Core plugin is embedded in the runtime and requires
no separate plugin DLL. To build Fusion Cutter from source, see [CONTRIBUTING.md](../CONTRIBUTING.md).

## Original games and mod tools

Copy the x86 `dinput8.dll` and `FusionCutter.dll` beside the executable you run. For Battlefront II this is
normally the `GameData` directory:

```text
GameData/
    BattlefrontII.exe
    dinput8.dll
    FusionCutter.dll
```

Use the same layout beside `Battlefront.exe` for Battlefront I, `SPTest.exe` for its test executable, or
`BF2_modtools.exe` / `BF2_modtools_NoDVD.exe` for Battlefront II Mod Tools. Renamed executables also work:
Fusion Cutter identifies the running build from its executable layout.

Start the game normally. The proxy starts Fusion Cutter when the game creates its DirectInput interface,
then forwards DirectInput calls to Windows. A server launch that skips DirectInput needs the dedicated
server loader described below.

### BF2GameExt

Keep Fusion Cutter's `dinput8.dll` and place `BF2GameExt.dll` and `BF2GameExt.ini` beside it. A separate
GameExt proxy is unnecessary. The loader honors `[General] Enabled` in `BF2GameExt.ini`, defaulting to enabled.

GameExt initializes before the game's startup constructors because some of its patches replace arrays
those constructors initialize. Fusion Cutter still starts when the game creates its DirectInput interface.

### Another mod already uses dinput8.dll

The Fusion Cutter proxy can forward calls through one adjacent DLL named `dinput8_*.dll`. For example, an
existing mod's proxy can be named `dinput8_other.dll`, leaving Fusion Cutter's proxy as `dinput8.dll`.
Check whether that mod supports being renamed.

Use only one matching forwarding DLL. Multiple matches are ignored and the proxy uses Windows DirectInput.
When updating Fusion Cutter, replace its old proxy; keeping that proxy as `dinput8_old.dll` would make it a
forwarding candidate.

## Classic Collection

Use the x64 files in the directory containing `Battlefront.exe` and the game's DLLs.

1. Rename the original `Battlefront1.dll` to `Battlefront1.original.dll` for Battlefront I, or
   `Battlefront2.dll` to `Battlefront2.original.dll` for Battlefront II.
2. Copy Fusion Cutter's corresponding `Battlefront1.dll` or `Battlefront2.dll` into that directory.
3. Copy `FusionCutter.dll` into the same directory.

Both games can be installed together and share one runtime:

```text
Battlefront.exe
Battlefront1.dll             # Fusion Cutter loader
Battlefront1.original.dll    # Original Battlefront I game DLL
Battlefront2.dll             # Fusion Cutter loader
Battlefront2.original.dll    # Original Battlefront II game DLL
FusionCutter.dll
```

Keep the proxy and `.original.dll` filenames shown above. Each proxy loads its adjacent original game DLL,
starts Fusion Cutter and forwards the game's entry point. When updating Fusion Cutter, replace the proxy
and runtime while keeping the original game DLL. After a game update, use the new original game DLL and
check that its build is supported.

## Dedicated servers

Use `RconServer_32.dll` for an x86 Battlefront I or II process, or `RconServer_64.dll` for either Classic
Collection game. These DLLs contain both the loader and runtime, so this route needs only one Fusion Cutter DLL.

Configure your server launcher or injector to load the DLL into the server process and keep it loaded.
Copying it beside the game alone does not start Fusion Cutter. In Classic Collection, the loader waits for
either game's DLL to load before starting the runtime.

This loader starts Fusion Cutter with **Role: Server**. The client loaders use **Role: Client**, including
when a player hosts a game. Use one loader route per process.

The loader filename and the Core plugin's `RconServer` patch refer to different components. Loading
`RconServer_32.dll` or `RconServer_64.dll` starts the framework; the patch provides remote administration only
on its supported Battlefront II builds.

## Additional plugins

Place plugin DLLs in a `plugins` directory beside the runtime. For a dedicated server, that means beside
`RconServer_32.dll` or `RconServer_64.dll`:

```text
FusionCutter.dll
plugins/
    ExamplePlugin.dll
```

Plugins must match the process architecture and the runtime's application binary interface (ABI), the
contract used to call between DLLs. Follow the plugin's installation instructions for any additional files.
Fusion Cutter examines every DLL directly inside `plugins` as a plugin; it does not search subdirectories.

Restart the game after adding, replacing or removing a plugin.

## Recognized builds

These are the game profiles in the current image catalog. An image is a loaded executable or DLL; its profile
identifies a particular build. Fusion Cutter checks its executable layout. The running executable can have
any name; supplemental DLLs use the listed lookup names. Individual patches also check the game code or
data they use.

### Battlefront I builds

| Build | Profile ID | Architecture | Filename |
| --- | --- | --- | --- |
| Steam | `BF1.Steam.5F243336` | x86 | `Battlefront.exe` |
| DVD | `BF1.DVD.41E48CFC` | x86 | `Battlefront.exe` or `Battlefront SWBFSpy.exe` |
| SPTest | `BF1.SPTest.41C75543` (December 2004)<br>`BF1.SPTest.41EC6058` (January 2005) | x86 | `SPTest.exe` |
| Classic Collection | `BF1.Classic.66702CCB` | x64 | `Battlefront1.dll` or `Battlefront1.original.dll` |

### Battlefront II builds

| Build | Profile ID | Architecture | Filename |
| --- | --- | --- | --- |
| Steam | `BF2.Steam.59EDE353` | x86 | `BattlefrontII.exe` |
| GOG | `BF2.GOG.59EDF52B` | x86 | `BattlefrontII.exe` |
| DVD | `BF2.DVD.44961FB6` | x86 | `BattlefrontII.exe` or `BattlefrontII_DVD.exe` |
| ModTools | `BF2.ModTools.43EBD102` | x86 | `BF2_modtools.exe` or `BF2_modtools_NoDVD.exe` |
| Classic Collection | `BF2.Classic.66702CD2` | x64 | `Battlefront2.dll` or `Battlefront2.original.dll` |

The DVD profile covers the executable without SecuROM. The ModTools profile covers the NoDVD executable,
including renamed copies.

Both Classic Collection games share the x64 launcher `Battlefront.exe`, recognized as
`Classic.Bootstrap.66702CD7`. The game DLL profiles above select patches for each game separately.

## Feature support

The columns below refer to the exact builds listed above. **Client**, **Server** and **Both** identify the
supported launch roles; **—** means unsupported. A recognized build alone does not establish that a feature
has been tested in play. Additional plugins declare their own supported builds.

### Battlefront I features

| Feature | Steam | DVD | SPTest | Classic Collection |
| --- | --- | --- | --- | --- |
| Architecture | x86 | x86 | x86 | x64 |
| Runtime and plugin loading | Both | Both | Both | Both |
| Lua bindings | — | — | — | — |
| Lua multiplayer | — | — | — | — |
| Game context | — | — | — | — |
| Multiplayer messaging | — | — | — | — |
| Lobby publishing | — | — | — | — |

### Battlefront II features

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
values require Galaxy.

See [Included patches](../README.md#included-patches) for the other Core patches and their supported builds.

## Check the installation

After starting the game, open `FusionCutter.txt` beside the runtime. Check the version, executable, build and
role, then read any **Issues**. Classic Collection reports the launcher build in the Game section; patches
select the relevant game DLL separately.

`FusionCutter.ini` is created on startup. Plugin INI files are created only when there are applicable settings
to write. For example, the current Core plugin produces no `FC.Core.ini` for Battlefront I.
See [configuration](configuration.md) for the file layout and [troubleshooting](troubleshooting.md) for failures.

## Update or remove

Update the loader and runtime together while the game is closed. Keep your INI files; Fusion Cutter adds
missing settings when it starts. Keep matching debug symbols if you need to investigate a crash.

To remove the original-game installation, remove Fusion Cutter's `dinput8.dll` and `FusionCutter.dll`.
Restore another mod's proxy to its original name if you renamed it for forwarding.
For Classic Collection, remove each Fusion Cutter game proxy and rename its `.original.dll` back to the
original name. For a dedicated server, remove the launcher or injector's instruction to load the RconServer DLL.

Configuration, logs, traces and plugin files can then be removed if they are no longer needed.
