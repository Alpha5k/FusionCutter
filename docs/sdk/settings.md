# Declaring settings

Settings let players change a patch's behavior without rebuilding its DLL. You describe the editable values
and defaults once; FC generates applicable INI entries, reads overrides and reports invalid input. The handler
receives a typed C++ object instead of parsing strings or opening configuration files itself.

A **schema** is the description connecting public setting names to that object's fields. It includes their
types, descriptions and any validation. A schema does not contain live game state. FC captures configuration
at startup, so changing a file affects the next game launch.

Use a setting for a choice the player or server operator should make, such as an interval or display option.
Keep a game's addresses and structure offsets in the patch's layout. They describe the supported binary,
and exposing them as user options would bypass the compatibility assumptions the patch relies on.

## Connect configuration to a C++ object

Include `<fc/settings.hpp>` or `<fc/plugin.hpp>`. Define a settings struct with ordinary C++ defaults, then
bind its fields with `fc::setting`. `fc::settings<T>` captures a value-initialized default object and owns
the bindings. The handler's `Settings` alias must match its effective schema.

```cpp
#include <fc/plugin.hpp>

namespace example {

enum class DisplayMode {
    Compact,
    Detailed
};

struct Settings {
    int interval_ms = 250;
    bool show_peers = true;
    std::string label = "Session";
    DisplayMode display = DisplayMode::Compact;
    fc::StringMap colors{{"Friendly", "00FF00"}, {"Enemy", "FF0000"}};
};

// Declares each editable field while retaining Settings' ordinary defaults.
fc::TypedSettingsSchema<Settings> schema() {
    return fc::settings<Settings>({fc::setting("IntervalMS", &Settings::interval_ms, "Refresh interval in milliseconds.")
                                       .range(50, 5000)
                                       .environment("EXAMPLE_INTERVAL_MS"),
                                   fc::setting("ShowPeers", &Settings::show_peers, "Show the current peer count."),
                                   fc::setting("Label", &Settings::label, "Heading displayed in status."),
                                   fc::setting("Display", &Settings::display, "Amount of detail displayed.")
                                       .choices({{"Compact", DisplayMode::Compact}, {"Detailed", DisplayMode::Detailed}}),
                                   fc::setting("Friendly", &Settings::colors, "Six-digit RGB color.").section("Colors"),
                                   fc::setting("Enemy", &Settings::colors, "Six-digit RGB color.").section("Colors")});
}

} // namespace example
```

Assign the result to `Patch::settings`, or call `.settings(schema())` on a variant to replace the entire
schema. A variant's replacement is not merged field by field. Each handler type must match the schema
selected for it, even when two schemas happen to contain similar fields.

The example describes a configurable monitor. `IntervalMS` controls its refresh interval, `ShowPeers` and
`Display` control presentation, and `Label` supplies text. The `Colors` subsection holds two named strings.
The schema only reads and validates these choices; the handler must use them to implement the monitor.

For a patch named `Monitor`, its settings would appear as follows, alongside its separately generated toggle:

```ini
[Monitor]
IntervalMS = 250
ShowPeers = true
Label = "Session"
Display = "Compact"

[Monitor.Colors]
Friendly = "00FF00"
Enemy = "FF0000"
```

The C++ member `interval_ms` and the INI key `IntervalMS` need not have the same spelling. The member pointer
in `fc::setting` connects them. Attach this schema to the patch, declare `using Settings = example::Settings`
in its handler, and accept the object in the constructor as shown in [getting started](getting_started.md).
Copy the fields needed after construction.

`environment("EXAMPLE_INTERVAL_MS")` is useful when a launcher or server environment supplies the interval.
For instance, an override of `1000` wins over `IntervalMS = 250`. An override outside the declared range warns
and selects the default 250, rather than trying the INI value next.

## Field types and modifiers

Supported ordinary fields are booleans, signed and unsigned integers, finite floating-point numbers,
`std::string`, named enums and declared `StringMap` entries. Numeric conversion checks the destination
type's range. Text fields own their reconstructed strings.

| Builder operation | Meaning |
| --- | --- |
| `setting(key, &T::member, description)` | Bind a field; description is optional |
| `.section(name)` | Write the field under `[PatchId.name]`; omission uses `[PatchId]` |
| `.environment(name)` | Declare an environment override for this field |
| `.range(minimum, maximum)` | Inclusive numeric bounds |
| `.choices({{name, enum_value}, ...})` | Enum values represented as named text, matched ASCII case-insensitively |
| `.validate(callback)` | Additional scalar validation after conversion and range checks |

Builder setters replace that constraint or metadata when called again. Enum defaults need a matching named
choice; choice names must be nonempty and unique. Defaults are validated too, so an invalid default rejects
the declaration instead of becoming a usable fallback.

`setting` returns `SettingBuilder<T, MemberType>`. Each modifier returns that builder by reference for
chaining; conversion to `Setting<T>` copies its metadata and validation callback. `settings<T>({fields})`
returns `TypedSettingsSchema<T>`, which converts to the `SettingsSchema` stored by Patch or Variant.

Malformed author declarations throw `std::invalid_argument`: reversed or non-finite range bounds, duplicate
choice names, a default without an enum choice, duplicate StringMap keys, or a map binding without a default
entry. `FC_PLUGIN` reports factory exceptions as plugin export failures. Invalid player input instead follows
the per-setting fallback described below.

A scalar validator returns `Result<>`; text validators receive `std::string_view`, other ordinary fields
receive their value type. The text view lasts for that call:

```cpp
fc::setting("Label", &Settings::label, "Heading displayed in status.")
    .validate([](std::string_view value) -> fc::Result<> {
        if (value.empty()) {
            return std::unexpected(fc::Error{"Label must not be empty"});
        }
        return {};
    });
```

Keep setting descriptions to one or two useful INI comment lines. Write explicit `\n` where the generated
description needs a line break. Use PascalCase keys and state units and special values.

## StringMap

`StringMap` is a finite set of named string defaults. A binding's key selects an existing entry in the map;
it does not make every arbitrary INI key part of the map.

Use it when the patch works with named values as a collection, such as colors for known chat categories.
The Colors example can look up `Friendly` by name rather than declaring one C++ member per color. The two
bindings still define exactly which entries are editable. Use ordinary members for unrelated options with
different types or meanings.

| Method or type | Meaning |
| --- | --- |
| `StringMap::Entry` | Owned `key` and `value` strings |
| `StringMap({entries})` | Preserve declaration order; duplicate keys are invalid |
| `find(key)` | Optional borrowed string view; ASCII case-insensitive lookup |
| `entries()` | Borrowed span in declaration order |
| `assign(key, value)` | Replace an existing entry; a missing key throws |

The map must outlive its returned views. Bind each editable map entry explicitly, as in the Colors example.
Copy a returned string before calling `assign` on its entry or replacing the map; those changes can
invalidate the view. `assign` changes the C++ object, not the player's INI file.

## Validate the completed object

Use a completed-object validator when settings interact or need normalization. Assign a function pointer
to `TypedSettingsSchema<T>::validate` before converting it to `SettingsSchema`.

A range check can establish that each timing value is between 1 and 10000. It cannot establish that the
minimum is no greater than the maximum, because that requires both fields. That is the distinction between a
scalar validator and this final validation step:

```cpp
struct Timing {
    int minimum_ms = 100;
    int maximum_ms = 500;
};

// Rejects an interval range whose endpoints are reversed.
fc::Result<> validate_timing(Timing &value) {
    if (value.minimum_ms > value.maximum_ms) {
        return std::unexpected(fc::Error{"MinimumMS exceeds MaximumMS"});
    }
    return {};
}

auto schema = fc::settings<Timing>({
    fc::setting("MinimumMS", &Timing::minimum_ms).range(1, 10000),
    fc::setting("MaximumMS", &Timing::maximum_ms).range(1, 10000)
});
schema.validate = &validate_timing;
```

This runs once on the reconstructed object before handler construction. It may modify that object, for
example to normalize a path. Rejecting it fails the selected patch; it does not reset every field to defaults.

For a single invalid input such as an unknown enum name, falling back to that field's default is well defined.
For incompatible values such as reversed endpoints, silently choosing one field to reset could change the
user's intent. The completed-object validator lets the patch explain that combination as a startup error.

## Configuration behavior

The runtime captures applicable settings at startup, including variants waiting for DLLs. Environment input
wins over INI when declared. Invalid selected input warns and falls directly to the field's declared default.
A malformed or unreadable file rejects the plugin; a bad individual value does not.

`enabled` and `configurable` are separate controls, not scalar schema fields. Groups can supply enabled
state only to non-configurable members. Full precedence and file syntax are in
[configuration](../configuration.md).

`Setting<T>` stores a lowered field's metadata and conversion callbacks. `TypedSettingsSchema<T>` owns
the typed model and optional completion validator; `SettingsSchema` retains it after type erasure inside
the plugin. Normal declarations use the builders above rather than constructing these records manually.
