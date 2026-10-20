# Troubleshooting

Start with `FusionCutter.txt` in the runtime directory. Check **Started** to confirm it describes the launch
you are investigating, then read **Issues** and **Diagnostics**. The runtime directory is beside
`FusionCutter.dll`, or beside the dedicated `RconServer_32.dll` / `RconServer_64.dll`.

## Diagnostic files

| File | What it contains |
| --- | --- |
| `FusionCutter.txt` | Current game information, plugin outcomes, patch status and diagnostic output state |
| `FusionCutter.log` | Events allowed by the configured log level |
| `FusionCutter.1.log`, `FusionCutter.2.log` | Rotated log backups; each log is limited to 4 MiB |
| `FusionCutter.crash.log` | Exception or fatal-error reports, including addresses and available patch attribution |
| `traces/FusionCutter-*.etl` | Binary trace events submitted during one session |

Some files appear only when there is data to write. The default log level is `Warning`, so a quiet log does
not by itself mean Fusion Cutter failed to start. A trace file appears only after trace data is submitted.
Check timestamps before treating an existing crash report or log entry as part of the current problem.

For more detail, edit `FusionCutter.ini` and restart:

```ini
[FusionCutter]
LogLevel = "Debug"
```

Change the existing `LogLevel` entry rather than adding a duplicate key. Debug logging records configuration
sources, selection decisions and lifecycle details. Return to your normal level after collecting the issue.

## Reading the status file

**Game** identifies the executable and its recognized build. **Role** is the fixed Client or Server role chosen
by the loader. **Mode** describes the current session:

| Mode | Meaning |
| --- | --- |
| `None` | Game context is available, with no active session or local mission |
| `Local` | A local mission is active |
| `Host` | This player is hosting a multiplayer session |
| `Client` | This player joined a multiplayer host |
| `Dedicated` | A dedicated multiplayer session is active |
| `Unavailable` | Fusion Cutter cannot currently determine the mode |

A client launch can become a Host without changing Role. `Unavailable` differs from `None`: it can mean the
build has no implementation for game context or that its integration failed. Check Issues and the
[build support tables](installation.md#feature-support).

**Plugins** summarizes installed, disabled, waiting and failed patches, and those that do not apply to this
launch. Disabled patches are listed by name. **Issues** explains failures and unresolved requirements.
**Patch Status** appears when an installed patch supplies additional information. **Diagnostics** shows the
log level, tracing state and any reported output failures or lost records.

## Fusion Cutter does not start

Check the [installation layout](installation.md) for your loader:

- Original games need the x86 `dinput8.dll` and adjacent `FusionCutter.dll`. The game must reach DirectInput
  creation for this route to start. A headless server may need the dedicated loader.
- Classic Collection needs the correct x64 game proxy, its adjacent `.original.dll`, and `FusionCutter.dll`.
  The original file must be the game's DLL, not an older Fusion Cutter proxy.
- A dedicated server launcher or injector must actually load the RconServer DLL and keep it loaded.

Early loader failures may write only `FusionCutter.txt` beside the loader because normal logging has not
started. Read that file for the failed operation and Windows error. Check that the files belong to the same
Fusion Cutter build and process architecture.

## A patch is missing, waiting or failed

**Not applicable:** the patch does not match this executable or launch role. For example, Battlefront II
patches do not apply to Battlefront I. They should not generate configuration for that executable.

**Waiting:** a required DLL or dependency is not ready. A DLL loaded later by the game can resolve this
automatically. If it stays waiting after entering the relevant game feature, use Debug logging to identify
the requirement and compare it with the supported build list.

**Disabled dependency:** enable the required patch or group through its documented control, then restart.
Enabling a consumer never enables its dependencies automatically.

**Unrecognized build or unexpected code or data:** the loaded image or bytes at a patch location differ
from what Fusion Cutter expects. Compare the reported profile, image and location with the actual game
files. A game update or another modification can explain a difference, but the mismatch alone does not
identify its cause. Preserve the complete error, including expected and observed bytes, for investigation.

**Conflicting edits:** two plans claim incompatible changes to a location. Use the reported patch
names and location to identify the combination. Disable an optional conflicting patch through its settings
or remove its external plugin, then restart.

**Plugin rejected:** read the first error for that DLL or plugin ID. Common causes include the wrong
architecture, an incompatible ABI, a missing `FC_GetPlugin` export, invalid declarations or a broken INI file.
Only actual plugin DLLs belong directly in `plugins`; every DLL there is examined as a plugin.

## Configuration is missing or ignored

Check the directory listed in `FusionCutter.txt` and the exact filename: plugin configuration uses
`FC.<PluginId>.ini`. Restart after editing; settings are captured at startup.

An absent `FC.Core.ini` can be expected when Core has no applicable settings for that build or role.
Non-configurable patches also have no toggle. Adding a key cannot make an incompatible or non-configurable
patch available.

If a value falls back to its default, check its type, units and allowed range. A declared environment variable
can override an INI setting. For rejected files, check for duplicate keys, malformed sections or quoted text,
and whether the file can be read. See [configuration](configuration.md) for exact syntax and failure behavior.

Entries from older installations are preserved. Remove obsolete keys after checking the warning and the
current plugin's settings.

## Logging or tracing reports a problem

An output failure includes the failed operation and, when available, a Windows error. Check that the runtime
directory is writable and the disk has space. Lost-record counters mean the output is incomplete; include
them when sharing a report.

Tracing states distinguish these cases:

- **disabled:** `MaxTraceSizeMB` is `0`.
- **no data submitted:** no trace file has been needed yet.
- **recording:** the session has a trace file.
- **stopped for size or disk limit:** recording stopped after reaching the configured budget or a disk limit.
- **failed:** trace output encountered another failure; read the log for its reason.

Increase `MaxTraceSizeMB` before the next launch if a trace needs more room. ETL files contain binary events;
their payloads are defined by the patches that wrote them. Interpreting those payloads requires the relevant
patch's trace format or analysis tools.

## Checking files without launching the game

If you have `FusionCutterVerify.exe`, use the build matching the target architecture. For example, from its
directory in PowerShell:

```powershell
.\FusionCutterVerify.exe --image "BF2.Steam.59EDE353=C:\Games\Battlefront II\GameData\BattlefrontII.exe" --check Core.ColoredChats
```

Replace the profile and path with the actual build. Repeat `--image ProfileId=Path` for required DLLs, add
`--plugin Path` for an external plugin, or `--dedicated` to select the Server role. `--config Directory` reads
configuration from that directory; `--json Path` writes a structured report.

`--check Plugin.Patch` requires that named patch to pass instead of merely appearing in the report. Read
the checks marked `Not checked` as well as failures: successful static validation does not exercise installed
hooks, gameplay or remote peers.

## Reporting a problem

Include a short description of what you expected, what happened and how to reproduce it, along with:

- The Fusion Cutter version, game build, launch role and relevant plugin versions.
- `FusionCutter.txt`, the relevant log and rotated logs if the event moved into them.
- The affected settings, including any environment overrides.
- For a crash, `FusionCutter.crash.log` and the matching build's debug symbols when available.
- For a trace investigation, the session's ETL file and any reported losses.

The crash reporter can observe an exception before the game handles it, so an exception report alone does
not establish that the process crashed. State whether the game continued, closed or stopped responding.
Describe where the issue occurred in the session so it can be matched to the recorded events.
