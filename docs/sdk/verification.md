# Verifying and debugging a plugin

Verification answers several different questions: whether the plugin uses the SDK correctly, whether its
declarations and game addresses fit a game build, and whether its installed behavior works in the game.
Each check provides different evidence. A successful C++ build cannot establish that an address denotes
the intended game function, and a matching instruction cannot establish that the callback behaves correctly.

| Check | What it can establish |
| --- | --- |
| Compile and export checks | The code builds for the target architecture and exposes the plugin entry point |
| Tests of patch-owned logic | Settings interactions, parsing or calculations behave as intended for the cases tested |
| Offline verifier | The supplied images, declarations, settings and patch plans pass applicable static checks |
| Binary inspection | Game addresses, instructions and calling conventions agree with the intended operation |
| Live game testing | Installed callbacks and behavior work under the tested game conditions |

Build the plugin for each architecture you distribute. Keep the matching DLLs, PDBs, plugin version and
game profile IDs together so a report can be traced to its code and game layouts.

## Compile and inspect

The SDK checks handler signatures, settings types, callback `noexcept` requirements and many contract types
at compile time. Export construction also checks declaration details such as required enabled defaults and
schema/handler agreement. The runtime then validates the resulting declarations.

Confirm the DLL exports the undecorated `FC_GetPlugin` name. `FC_PLUGIN(factory)` supplies the appropriate
export for x86 and x64. An architecture mismatch, missing export or incompatible ABI prevents admission.

Keep automated tests focused on behavior your patch owns: settings interactions, protocol encoding,
calculations and decisions that can be exercised without a game. The framework's own tests do not establish
that a new game's address or calling convention is correct.

## Offline verifier

The verifier runs the checks it can perform against game files without launching a playable game. It is
useful while developing a patch for several builds: an address or expected-byte mismatch can be found before
installing the DLL in each game directory. It uses the same declarations and plan code as the runtime, so
keep Plan limited to its supported operations rather than assuming the game is already executing.

Use `FusionCutterVerify.exe` from the SDK matching the target architecture. It includes Core and loads the
external plugins explicitly supplied on the command line. For example:

```powershell
.\FusionCutterVerify.exe --image "BF2.Steam.59EDE353=C:\Games\Battlefront II\GameData\BattlefrontII.exe" --plugin ".\ExamplePlugin.dll" --check ExamplePlugin.Status --json ".\verification.json"
```

This command identifies one game file, loads the [first tutorial's plugin](getting_started.md), requires
`ExamplePlugin.Status` to be checked and saves the result as JSON. Replace the paths with your actual files.
The named check matters: otherwise a plugin with no applicable patch could produce no failed checks while
leaving the feature you intended to test unchecked.

| Argument | Meaning |
| --- | --- |
| `--image ProfileId=Path` | Required at least once; repeat for every image the plan needs |
| `--plugin Path` | Load one external plugin; repeat for additional plugins |
| `--config Directory` | Read existing runtime and plugin INI files from this directory |
| `--dedicated` | Select Server role; omission selects Client |
| `--check Plugin.Patch` | Require that patch to be enabled and complete applicable plan validation |
| `--json Path` | Write the structured report in addition to console output |

Use the exact profile for each file. A multi-image variant needs its game and supporting DLL inputs.
Configuration is read without creating or extending files. Without supplied files, defaults and declared
environment overrides apply; the report notes missing configuration coverage.

Read all three report outcomes:

- **Passed:** the named check completed successfully.
- **Failed:** a required condition or operation failed.
- **Not checked:** the check lacked inputs or is outside offline verification.

The exit code is 0 with no failed checks, 1 when validation checks fail, and 2 for argument, input, output or
tool errors. Exit code 0 can still include Not checked items. `--check` makes a particular patch's absence,
disabled state or incomplete plan a failure instead of leaving it as a coverage gap.

The verifier checks image profiles, declarations, captured settings, selection and patch plans. It reports
operation locations and evidence. It does not execute Prepare or Activate, install hooks, validate live
mapping permissions, run game code, assess runtime service-hook conflicts, or prove multiplayer interoperability.

## Validate assumptions about game code and data

Check each supported build's actual bytes, decoded instruction boundaries, pointer destinations and calling
convention. For relocatable images, account for relocation in evidence and validate plans at another load
address where applicable. A filename or recognized image profile is not enough to validate a patch site.

Use target evidence for pointers or branches when their absolute values move with the module. When several
profiles share a handler, verify each selected layout. A wildcard selector also accepts later catalog entries
under that prefix, so its assumptions about game code and data must remain valid or fail clearly.

## Live checks

Live testing exercises what the verifier cannot: callback execution, game transitions, rendering or input
behavior, current memory permissions and interactions with other patches. For multiplayer, include a remote
client, a playing host and a dedicated host when the feature supports them; local delivery alone does not
exercise the remote transport.

For Lua, check existing and newly created VMs, callback failures, retained references after VM teardown and
deferred work. For late DLL requirements, check installation when the image appears after startup.

Report automated, static and live results separately. Keep known gaps explicit. Use Debug logging and
targeted trace channels to collect the facts needed for a failed case, then inspect the complete messages
and status output as part of the feature's review.

## Follow the first failed stage

If the DLL never appears in FC's reports, start with installation, architecture and the export. If the plugin
appears but its patch is unavailable, inspect the selected profile and launch role. If it is enabled but cannot
install, inspect configuration, dependencies and the first Plan or Prepare error. If it is installed but its
behavior is missing, inspect the relevant live callback, session state or remote feature agreement.

That order helps distinguish, for example, an incorrect variant from a correctly installed multiplayer patch
waiting for a compatible peer. It also keeps callback changes from hiding a declaration problem. The
[troubleshooting guide](../troubleshooting.md) explains the runtime's reports; the
[reporting guide](reporting.md) explains what diagnostic information your patch can add.

## Debug a callback

A PDB maps compiled instructions to function names and source locations. Use the PDB produced with the
installed DLL and the source from that build. A PDB from a later rebuild may not match, even with unchanged
source. Visual Studio checks that the symbols match; its
[symbol guide](https://learn.microsoft.com/en-us/visualstudio/debugger/specify-symbol-dot-pdb-and-source-files-in-the-visual-studio-debugger)
explains how to load them.

For a callback that runs after startup:

1. Install the DLL you intend to investigate, start the game through its normal loader, and check that
   `FusionCutter.txt` reports the patch installed.
2. Open the plugin's source in Visual Studio. Use **Debug > Attach to Process**, select the game process,
   and use the **Native** code debugger. Match the process by its executable path when several games are
   open. See Microsoft's [attach instructions](https://learn.microsoft.com/en-us/visualstudio/debugger/attach-to-running-processes-with-the-visual-studio-debugger).
3. Pause execution and open **Debug > Windows > Modules**. Find your plugin, check its loaded path, and
   load the retained PDB if symbols have not loaded. Load matching FC symbols too when investigating a
   runtime frame; a dedicated server needs symbols for its RconServer DLL.
4. Set a breakpoint inside the callback, resume, and trigger the relevant game action. For a hook, inspect
   its arguments, the selected thread and the call stack. For a Lua callback, invoke its registered function
   from a script in the relevant VM.
5. If the failure is an access violation, open **Debug > Windows > Exception Settings** and enable breaking
   on Win32 exception `0xC0000005`. Reproduce the failure to inspect the instruction and its inputs before
   exception handlers run. For a C++ exception, select the relevant C++ exception instead. Microsoft's
   [exception guide](https://learn.microsoft.com/en-us/visualstudio/debugger/managing-exceptions-with-the-debugger)
   explains these controls.

Breaking before exception handlers run can also stop on exceptions the game handles successfully.
Check whether the stopped operation corresponds to the failure you are investigating.

A breakpoint that cannot bind often means the DLL or symbols are not loaded, or the source differs.
If it binds but never runs, check installation, variant selection and whether the game reaches that callback.
Plan and Prepare run during startup; investigating those requires starting the process under the debugger
or attaching before FC reaches them.

`RelWithDebInfo` keeps optimization, so some locals may be unavailable and source stepping may skip lines.
A separate Debug build can make stepping easier, but reproduce against the distributed DLL first when the
failure depends on optimization or timing.

FC's `FusionCutter.crash.log` is a text report, not a process dump. Use its exception address and available
module or patch attribution to locate the failure. For a reported address within a module, subtract that
module's base from the same run to obtain its RVA; compare it with the matching binary. The report alone
cannot reconstruct all locals or establish that the named patch caused the fault. Preserve it with the
status file, logs, reproduction steps and matching build files.
