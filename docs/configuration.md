# Configuration

Fusion Cutter reads configuration once at startup. Close the game before editing settings and restart it
to apply changes.

Files are stored beside `FusionCutter.dll`, or beside the combined `RconServer_32.dll` / `RconServer_64.dll`
when using the dedicated loader. This is also the directory shown in `FusionCutter.txt`.

| File | Purpose |
| --- | --- |
| `FusionCutter.ini` | Runtime logging and tracing settings |
| `FC.<PluginId>.ini` | One plugin's patch controls and settings, such as `FC.Core.ini` |

The plugin ID comes from its declaration, so it may differ from its DLL filename.

## Runtime settings

The defaults are:

```ini
[FusionCutter]
LogLevel = "Warning"
MaxTraceSizeMB = 512
```

`LogLevel` selects which messages reach `FusionCutter.log`:

| Value | Messages included |
| --- | --- |
| `Off` | None |
| `Error` | Failures |
| `Warning` | Errors and warnings; the default |
| `Info` | Errors, warnings and normal operational events |
| `Debug` | All of the above plus details for troubleshooting |

Changing the log level does not disable status reports, crash reporting or tracing.

Tracing uses Event Tracing for Windows (ETW) to collect binary diagnostic events. `MaxTraceSizeMB` limits the
session's trace file. It accepts a nonnegative whole number, measured in MB where 1 MB is 1,048,576 bytes.
Set it to `0` to disable recording. Recording stops when the file reaches its limit; it does not overwrite
earlier events. A file is created only when a patch submits trace data.

## Patch controls and settings

Fusion Cutter writes settings for patches that match the current executable and launch role, including
patches waiting for a required DLL. Patches for another executable or role do not add settings. A plugin
with no applicable controls or settings needs no INI file.

Patch toggles appear under their category. Patch-specific settings appear under the patch's own section.
For example, this portion of `FC.Core.ini` enables chat colors and limits the rendered frame rate to 144 FPS
on a supported Battlefront II client:

```ini
[Client]
ColoredChats = true
UnlockFrameRate = true

[UnlockFrameRate]
MaxFrameRate = 144
```

The frame rate setting defaults to `120`; `0` removes the limit. Other factors, such as vertical synchronization,
can still limit the observed frame rate.

A patch without a category uses `Enabled` in its own section:

```ini
[ExamplePatch]
Enabled = true
```

Plugins can place related settings in sections such as `[ExamplePatch.Network]`. Keep the generated section
and key names, and use the setting's description for its meaning, units and allowed values.

### Groups and defaults

A group can control several patches. Its toggle uses the same format as a patch toggle: the group ID under
its category, or `Enabled` under the group ID when it has no category.

The effective enabled state follows these rules:

1. A patch with its own configurable toggle uses that setting, or its declared default when the setting is
   missing or invalid. A group does not override it.
2. Otherwise, a configurable group containing the patch supplies the group's setting or default.
3. Otherwise, the patch uses its own declared default.

A patch can be enabled without having an editable toggle. The plugin author chooses which controls are
configurable. Adding an undeclared `Enabled` setting does not create a control.

Dependencies do not enable patches. A required patch must already be enabled and compatible; depending on a
group requires every member. If a dependency is disabled or fails, the dependent patch cannot run.

### Environment overrides

A plugin may declare an environment variable for an individual setting. That variable takes precedence over
the INI value and is read at startup. An invalid environment value produces a warning and uses the setting's
default, even if the INI contains a valid value. Consult the plugin's documentation for declared variable names.

## INI syntax

- Save files as UTF-8 without a byte order mark. Put every key inside a `[Section]`.
- Section and key names are ASCII case-insensitive. Keep their generated spelling for consistency.
- Use `true` or `false` for booleans, decimal numbers for numeric settings, and no unit suffix in the value.
- Text can be unquoted or use JSON-style double quotes. For example, `Path = C:\Games\Battlefront` and
  `Path = "C:\\Games\\Battlefront"` represent the same text.
- Start a comment line with `;` or `#`. Inline comments are not supported; put comments on their own line.
- Repeated sections are merged, but a duplicate key in the same section rejects the file, even if its case differs.

Empty values are still values. To return a setting to its default, remove its key or copy the declared default;
leaving `Key =` may be invalid for that setting.

## Generated entries and errors

Fusion Cutter creates missing files when they have settings to write and adds missing entries to existing
files. It preserves existing values, comments and unknown entries. Unknown sections or keys produce warnings;
obsolete entries are left for you to remove.

If several possible variants declare different defaults for the same setting, the generated entry may be
commented out. Leaving it unset lets the selected variant supply its own default.

| Condition | Result |
| --- | --- |
| An individual value has the wrong type or fails validation | Warn and use that setting's declared default |
| A plugin INI is unreadable, malformed, or cannot be created when needed | Reject that plugin |
| `FusionCutter.ini` is unreadable, malformed, or cannot be created | Stop startup |
| Missing entries cannot be added to an otherwise readable file | Warn and continue using existing values and defaults |

Individual values can be valid while their combination is unusable. A plugin can reject that combination
when creating its patch. Patch failures follow the declared failure policy; a required patch with a fatal
policy can stop the process. Read `FusionCutter.txt` and the log for the affected item and reason.
