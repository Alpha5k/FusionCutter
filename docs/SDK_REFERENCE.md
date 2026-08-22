# FusionCutter SDK Reference

This document is the reference for the public FusionCutter SDK. It is organized by task so plugin authors can find
the appropriate framework capability without needing to read the SDK adapter implementation.

For a guided first plugin, see [Getting Started](GETTING_STARTED.md). For design and ownership guidance, see
[Plugin Best Practices](BEST_PRACTICES.md).

## SDK map

| Need | Use |
| --- | --- |
| Build an external plugin DLL | `fc_add_plugin()` |
| Define the plugin contribution | `fc::PluginDefinition`, `fc::plugin()` |
| Define a stateful patch | `fc::PatchDefinition`, `fc::patch<Handler>()` |
| Define a Plan-only patch | `fc::plan_patch()` |
| Declare supported targets | `fc::SupportDefinition`, `fc::support()` |
| Describe game locations | `DataLocation`, `FunctionLocation`, `CallLocation`, `CodeLocation`, `VtableLocation` |
| Validate expected native state | `Evidence` and its factory functions |
| Submit native work | `fc::Plan` |
| Share a physical hook | `Plan::hook()`, `Plan::observe()`, `Original`, `before()`, `after()` |
| Store native data referenced by a plan | `DataHandle`, `DataAddress`, `Plan::allocate_data()` |
| Read configuration | `SettingsSchema`, `value()`, `choice()`, `section()` |
| Connect patches across plugins | `InterfaceContract`, `InterfaceQuery`, `find_interface()`, `Plan::bind()` |
| Log patch activity | `Logger` |
| Publish current status | `StatusWriter` |
| Record high-volume diagnostics | `TraceChannel` |
| Validate a plugin without mutation | `fc::test::Scenario` from `FusionCutter/Testing.hpp` |

## Package and headers

An installed SDK package is consumed with:

```cmake
find_package(FusionCutterSDK CONFIG REQUIRED)
```

It provides these targets and helpers:

| Target or helper | Purpose |
| --- | --- |
| `FusionCutter::SDK` | Header-only C++23 target for patch authors |
| `FusionCutter::Testing` | Optional architecture-specific non-mutating test support |
| `fc_add_plugin()` | Builds an external plugin and generates its query export bridge |
| `fc_add_plugin_tests()` | Builds Catch2 tests and stages their plugin DLLs |

The package contains five public headers:

| Header | Intended user |
| --- | --- |
| `FusionCutter/SDK.hpp` | Ordinary C++ plugin authors; this is the primary SDK header |
| `FusionCutter/Testing.hpp` | Plugin tests using the non-mutating production validation path |
| `FusionCutter/Abi.h` | Shared C ABI primitives and target constants |
| `FusionCutter/PluginApi.h` | Manual or non-C++ plugin adapters; ordinary C++ authors do not include it |
| `FusionCutter/CoreApi.h` | FusionCutter loaders; it is not an API for patch authors |

The SDK supports Visual Studio 2022, MSVC v143, C++23, x86, and x64. A plugin DLL must match the architecture of the
FusionCutter runtime that loads it.

### `fc_add_plugin`

```cmake
fc_add_plugin(<target>
    FACTORY <qualified_name>
    SOURCES <source...>
)
```

`FACTORY` names a no-argument function with external linkage that returns `fc::Plugin`. The helper creates a Windows
module DLL, links `FusionCutter::SDK`, applies the supported compiler settings, and generates the required
`FusionCutter_QueryPlugin` export.

Do not also use `FC_EXPORT_PLUGIN` when building through `fc_add_plugin()`. `FC_EXPORT_PLUGIN(factory)` is the manual
export option for a build system that does not use the helper.

### `fc_add_plugin_tests`

```cmake
fc_add_plugin_tests(<target>
    PLUGINS <target...>
    SOURCES <source...>
)
```

The helper creates a Catch2 executable, links `FusionCutter::Testing`, stages the named plugin DLLs in a local
`plugins` directory, and registers the cases with CTest. The plugins and test executable must use the same
architecture. `FusionCutter::Testing` is test-only and must not be linked by a deployed plugin.

`fc_bundle_plugin()` belongs to FusionCutter source builds. It is not part of the installed SDK and does not change the
plugin's metadata or runtime model.

## Fundamental types

All C++ authoring types are in namespace `fc`.

| Type | Meaning |
| --- | --- |
| `Rva` | A 32-bit byte offset from the selected image base |
| `TargetInfo` | Selected layout, role, architecture, and concrete image profile ID |
| `Error` | Author-facing failure text: `message` and optional `operation` |
| `Result` | `std::expected<void, Error>` |
| `NativeData` | Concept accepting trivially copyable native values |
| `NativeFunction` | Concept accepting native function-pointer types |

The fundamental records expose these fields:

| Record | Fields |
| --- | --- |
| `Rva` | `std::uint32_t value` |
| `TargetInfo` | `layout`, `role`, `architecture`, and borrowed `image_profile` |
| `Error` | required `message` and optional `operation` attribution |

### Target enums

| Enum | Values |
| --- | --- |
| `TargetLayout` | `GameSpyRetail`, `SteamRetail`, `GOGRetail`, `ModTools`, `ClassicCollection` |
| `HostRole` | `Client`, `Server`, `All` |
| `TargetImage` | `Game`, `Bootstrap`, `GalaxyPeer` |
| `Architecture` | `X86`, `X64` |
| `FailurePolicy` | `Continue`, `Fatal` |
| `LogLevel` | `Off`, `Error`, `Warning`, `Info`, `Debug` |

`HostRole::All` is a mask used in support declarations. A runtime `TargetInfo::role` is always `Client` or `Server`.

The generation-1 support matrix is:

| Layout | Role | Patchable images |
| --- | --- | --- |
| GameSpy Retail | Client or Server | `Game` |
| Steam Retail | Client or Server | `Game` |
| GOG Retail | Client | `Game` |
| GOG Retail | Server | `Game`, `GalaxyPeer` |
| Mod Tools | Client | `Game` |
| Classic Collection | Client | `Bootstrap`, `Game` |
| Classic Collection | Server | `Game` |

`TargetInfo::image_profile` is the canonical ID of the selected physical image profile. Use it to choose between
reviewed revisions that share the same layout, role, and image tuple. Plugins do not declare profile IDs as support
metadata.

## Composition

A factory returns one owning `fc::Plugin`. The framework copies its definitions during admission.

### IDs

Plugin, patch, group, category, settings section, interface, and image profile IDs:

- contain 1-64 ASCII letters, digits, or underscores;
- begin with a letter;
- compare case-insensitively; and
- retain their declared spelling for output.

`FusionCutter` is a reserved plugin ID. `FusionCutter` and `General` are reserved patch, group, and category IDs.
The built-in `Core` plugin ID is also unavailable to external plugins. Patch and group IDs share one global catalog
namespace and must be unique across admitted plugins.

### Plugin definition

```cpp
struct PluginDefinition {
    std::string id;
    std::optional<std::string> version;
    std::optional<std::string> author;
    std::optional<std::string> source;
    std::vector<CategoryDefinition> categories;
    std::vector<GroupDefinition> groups;
    std::vector<Patch> patches;
};

fc::Plugin fc::plugin(PluginDefinition definition);
```

Every external or bundled plugin declares at least one patch. Only the built-in `Core` contribution may be empty.
`plugin()` validates author composition and throws `std::invalid_argument` for malformed definitions; the generated
registration bridge contains those exceptions.

### Categories and groups

```cpp
struct CategoryDefinition {
    std::string id;
    std::optional<std::uint32_t> order;
};

struct GroupDefinition {
    std::string id;
    std::vector<std::string> members;
    bool configurable = false;
    bool enabled = false;
    std::optional<std::string> category;
    std::optional<std::string> description;
};
```

Categories control presentation order. Groups provide presentation and optional shared selection; they do not become
runtime patch objects. Each group member must be a patch owned by the same plugin, and a patch may belong to at most
one group. A nonconfigurable group cannot set `enabled` to true.

### Patch definition

```cpp
template <class Settings>
struct PatchDefinition {
    std::string id;
    std::string name;
    bool enabled = false;
    bool configurable = true;
    std::optional<std::string> description;
    std::optional<std::string> version;
    std::optional<std::string> author;
    std::optional<std::string> source;
    std::optional<std::string> category;
    std::optional<SettingsSchema<Settings>> settings;
    std::vector<std::string> depends_on;
    std::vector<std::string> includes;
    FailurePolicy failure_policy = FailurePolicy::Continue;
    std::vector<Support> supports;
};
```

`id`, `name`, and at least one support are required. Omitted `enabled` means disabled; omitted `configurable` means
configurable. Optional patch version, author, and source override or supplement plugin-level presentation metadata.

Create a full handler patch with:

```cpp
fc::Patch patch = fc::patch<Handler>(definition);
```

Create a Plan-only patch with:

```cpp
fc::Patch patch = fc::plan_patch(definition, [](fc::Plan& plan) {
    // Submit planned work.
});
```

A settings-aware Plan callable has the form `void(fc::Plan&, const Settings&)`. Use a full handler when the patch needs
persistent mutable state, substantial resources, later lifecycle phases, status, or interfaces.

### Support definition

```cpp
template <class Settings>
struct SupportDefinition {
    std::vector<TargetLayout> layouts;
    HostRole roles;
    TargetImage image;
    std::optional<SettingsSchema<Settings>> settings;
    std::vector<std::string> depends_on;
    std::vector<std::string> includes;
    std::optional<FailurePolicy> failure_policy;
};
```

An untyped `fc::support({...})` inherits the patch handler and common settings schema. `fc::support<Handler>({...})`
selects a different handler for those target tuples and may provide a complete replacement schema. A support-specific
schema replaces the common schema; schemas are not merged.

At most one support may match a layout and runtime role. A patch instance owns one physical target image. A feature
that must patch two images uses two patches, normally within the same plugin.

`FailurePolicy::Continue` is the default. `Fatal` is for an essential startup patch whose failure must stop
initialization; it is invalid for a late-image support.

### Relationships

`depends_on` and `includes` contain patch or group IDs.

| Relationship | Selection | Ordering | Failure propagation |
| --- | --- | --- | --- |
| `depends_on` | Selects an applicable provider | Provider before consumer | Unavailable provider skips consumer |
| `includes` | Softly requests selection | None | Included patch outcome does not affect includer |

Neither relationship overrides an explicit configuration disable. Use `depends_on` only when the consumer cannot work
without the provider. Use `includes` for optional companion patches, including a companion waiting for a late image.

## Handler lifecycle

A handler is an ordinary class or struct. It has no FusionCutter base class and needs no virtual methods.

### Construction

A settings-free handler supports either:

```cpp
Handler();
Handler(const fc::CreateContext& context);
```

A typed handler declares its settings type and receives the completed settings object:

```cpp
class Handler {
  public:
    using Settings = PatchSettings;
    Handler(const fc::CreateContext& context, const Settings& settings);
};
```

The settings reference is valid only during construction. Store the values or derived state needed later. Constructors
establish lightweight state; substantial resources belong in the Prepare phase. Handler destruction must not throw.

### Optional methods

| Method | Purpose |
| --- | --- |
| `void plan(fc::Plan&)` | Describes coordinated native work without mutation |
| `fc::Result prepare(fc::PrepareContext&)` | Resolves planned storage and creates fallible substantial resources |
| `void activate(fc::ActivateContext&) noexcept` | Arms prepared behavior after the Commit phase |
| `void update(fc::UpdateContext&) noexcept` | Performs bounded, nonblocking work on the serialized pump |
| `void write_status(fc::StatusWriter&) const noexcept` | Writes a bounded current snapshot |
| `void query_interface(fc::InterfaceQuery&) noexcept` | Supplies copied interface contracts |

The SDK detects these exact method names and diagnoses an incorrect signature. Missing methods use the ordinary no-op
or success behavior. The framework owns native installation and rollback; a handler destructor releases only
plugin-owned resources.

### Phase contexts

| Context | Methods | Retention |
| --- | --- | --- |
| `CreateContext` | `target()`, `logger()` | Copy `TargetInfo` or `Logger`; do not retain the context |
| `Plan` | `target()`, `logger()`, Plan operations | Plan submissions and symbolic addresses are callback-scoped |
| `PrepareContext` | `logger()`, `resolve()`, `find_interface()`, `create_trace()` | Do not retain the context |
| `ActivateContext` | `logger()` | Do not retain the context |
| `UpdateContext` | `logger()` | Do not retain the context |

The Plan and Prepare phases may fail. The Activate and Update phases are `noexcept` and have no framework failure
return.

## Settings

Typed settings use the same schema for defaults, generated configuration, INI parsing, environment overrides,
validation, and handler construction.

### Supported values

`fc::value()` supports:

- `bool`;
- standard signed or unsigned non-character integer types;
- `float` and `double`; and
- `std::string`.

`fc::choice()` maps stable string spellings to a patch-defined enum. A finite `StringMap` section declares a fixed set
of string keys.

### Basic schema

```cpp
struct PatchSettings {
    std::uint32_t limit;
    bool diagnostics;
};

auto patch_settings() {
    return fc::settings<PatchSettings>(
        fc::value("Limit", &PatchSettings::limit, std::uint32_t{128})
            .range(std::uint32_t{1}, std::uint32_t{4096})
            .description("Maximum number of entries"),
        fc::value("Diagnostics", &PatchSettings::diagnostics, false));
}
```

### Schema types and helpers

| Type or helper | Purpose |
| --- | --- |
| `SettingEntry<Settings>` | Owning normalized entry; normally produced by a helper |
| `SettingsSection<Settings>` | Named settings section and its entries |
| `SettingsSchema<Settings>` | Root entries, named sections, and optional validator |
| `SettingsValidator<Settings>` | Alias for `fc::Result (*)(Settings&)` |
| `value()` | Boolean, numeric, or string setting |
| `choice()` | Enum setting with finite stable spellings |
| `settings<Settings>()` | Compact schema containing root entries |
| `section<Settings>()` | Named section containing ordinary entries |
| `section()` with `StringMap` member | Named finite string-map section |

Available modifiers are:

| Setting | Modifiers |
| --- | --- |
| Boolean | `.description()`, `.environment()` |
| Integer or floating | `.description()`, `.environment()`, `.range(minimum, maximum)` |
| String | `.description()`, `.environment()`, `.max_length(byte_count)` |
| Choice | `.description()`, `.environment()` |

`max_length` counts UTF-8 bytes and excludes a terminator. Zero means no patch-specific maximum. An environment name
declares the authoritative override for that setting; a present malformed value fails the patch rather than falling
back to the INI.

Setting keys are nonempty ASCII strings compatible with the generated INI syntax. They cannot begin with whitespace,
`;`, or `#`, end with whitespace, or contain a line break, `=`, or `:`. Keys compare case-insensitively within each
section.

### Choices

```cpp
enum class Policy {
    Disabled,
    Preferred,
    Required,
};

fc::choice("Policy", &PatchSettings::policy, Policy::Preferred,
           {{"Disabled", Policy::Disabled},
            {"Preferred", Policy::Preferred},
            {"Required", Policy::Required}})
```

Choice spellings are case-insensitively unique. The default enum value must appear in the declared choices.

### Named sections

```cpp
fc::SettingsSchema<PatchSettings> result;
result.sections = {
    fc::section<PatchSettings>(
        "Network",
        {fc::value("Port", &PatchSettings::port, std::uint16_t{3658})
             .range(std::uint16_t{1}, std::uint16_t{65535})}),
};
```

The generated section is `[PatchId.Network]`. A patch may have root settings under `[PatchId]` and any finite set of
one-level named sections.

### Finite string maps

```cpp
constexpr std::array kBindings{
    fc::StringSetting{.key = "A", .default_value = "Jump"},
    fc::StringSetting{.key = "B", .default_value = "Crouch"},
};

struct ControllerSettings {
    fc::StringMap bindings;
};

auto section = fc::section("Bindings", &ControllerSettings::bindings, kBindings);
```

`StringMap::find(key)` performs case-insensitive lookup and returns an optional string view. `entries()` returns the
resolved entries in declaration order. Returned views remain valid while the owning `StringMap` lives.

`StringSetting` declares `key`, `default_value`, optional `description`, optional `max_length`, and optional
`environment`. Each `StringMap::Entry` exposes the resolved `key` and `value` strings.

### Validation

Set `SettingsSchema::validate` to a function with this shape:

```cpp
fc::Result validate_settings(PatchSettings& settings);
```

Validation sees the complete typed object and may check relationships, normalize values, or compute lightweight
derived fields. It should not start workers, open files or sockets, or allocate substantial runtime resources; those
belong in the Prepare phase.

## Locations and evidence

Locations are plugin-owned semantic descriptions of addresses in the patch's selected image.

### Location types

| Type | Describes |
| --- | --- |
| `DataLocation<T, Count>` | One trivially copyable value or fixed contiguous array |
| `FunctionLocation<Call>` | A native function entry |
| `CallLocation<Call>` | One existing direct call instruction and its callee ABI |
| `CodeLocation` | Code bytes or an instruction boundary |
| `VtableLocation` | The beginning of a vtable |
| `VtableSlotLocation<Function>` | Alias for a typed function-pointer data location |

Every location has the same fields: `rva`, optional semantic `name`, optional native `label`, and optional `evidence`.
Use designated initialization whenever metadata is present.

`name` describes the location's role in the patch. `label` preserves a direct game or disassembler label. Neither is
an identity key.

### Derived locations

```cpp
fc::vtable_slot<Function>(table, index, metadata);
fc::element(array, index, metadata);
```

`LocationMetadata` contains optional `name`, `label`, and `evidence` for the derived location. Parent evidence is not
inherited. Invalid index or RVA arithmetic is preserved as a Plan failure rather than wrapping.

### Evidence

| Factory | Check |
| --- | --- |
| `exact_bytes(bytes)` | Exact nonempty byte sequence |
| `masked_bytes(bytes, mask)` | Bits selected by a nonempty mask |
| `expect(value)` | Exact object representation of a `NativeData` value |
| `points_to(location)` | Pointer-sized stored address targets the location |
| `direct_call_to(location)` | Existing supported direct call targets the location |
| `direct_jump_to(location)` | Existing supported direct jump targets the location |

Evidence owns its copied input. It verifies expected native state; it does not discover an address. Evidence is
optional, but omitting it skips comparison against that state and should be a deliberate decision. Use separate
`Plan::require()` operations for independent facts rather than trying to combine unrelated evidence.

## Native calls

Ordinary nonvariadic native function pointers cover compiler-supported `cdecl`, `stdcall`, `fastcall`, `thiscall`, and
Windows x64 calls. Use `fc::NativeCall<Signature, Layout>` only when the game's physical argument or result homes do
not match a compiler convention.

```cpp
using FinalSend = fc::NativeCall<
    int(int, const void*, int) noexcept,
    fc::abi::x86<
        fc::abi::args<fc::abi::ecx, fc::abi::edx, fc::abi::stack<0>>,
        fc::abi::result<fc::abi::eax>,
        fc::abi::caller_cleanup>>;
```

| ABI helper | Purpose |
| --- | --- |
| `abi::args<Homes...>` | Positional argument homes |
| `abi::result<Home>` | Direct result home |
| `abi::hidden_result<Home>` | Hidden result-pointer home for a record result |
| `abi::stack<Offset>` | Normalized stack-argument byte offset |
| `abi::x86<Arguments, Return, Cleanup>` | x86 physical layout |
| `abi::x64<Arguments, Return, Cleanup>` | Windows x64 physical layout |
| `abi::caller_cleanup`, `abi::callee_cleanup`, `abi::no_cleanup` | Stack cleanup rule |

Register tags include x86 general registers, `st0`, x64 general registers, and `xmm0` through the architecture's
supported range. The first home in `abi::args` belongs to the first logical argument. `abi::stack<0>` means the first
normalized stack argument, not the entry stack pointer.

A compiler-native variadic function may be resolved with `FunctionLocation` and `Plan::require()`. It cannot be used
for redirects, hooks, observers, or `Original`.

## Plan operations

`fc::Plan` is the complete public surface for native work that FusionCutter must validate, coordinate, or own. A Plan
may contain requirements, mutations, symbolic storage, hook participation, and optional interface bindings. It does not
change game state while the callback is running.

The framework copies accepted submissions and later validates all selected patches together. Only after validation and
private preparation succeed does the framework commit native changes as one transaction. This is why author code must
not bypass the Plan with a separate writer, hook library, or executable allocator.

### Structured and compact forms

Most operation families have two authoring forms:

| Form | Example | Use |
| --- | --- | --- |
| Structured | `plan.write(location, replacement)` | Reusable semantic location with a type, name, label, and evidence |
| Compact | `plan.write_at(rva, replacement, evidence)` | A simple operation whose location is clearer inline |

Both forms produce the same operation, claims, validation, conflict handling, and transaction behavior. Prefer a
structured location when it is reused, carries a native type, or benefits from a meaningful name. Compact forms are not
an escape from evidence or validation.

### Context

| Method | Result |
| --- | --- |
| `target()` | Selected `TargetInfo` by value |
| `logger()` | Copyable patch-scoped `Logger` |
| `fail(message, operation)` | Explicitly fails the Plan with the first supplied reason |

The Plan and its symbolic addresses are callback-scoped. `target()` may be copied, and a returned `Logger`,
`DataHandle`, or `Original` may be retained where its own contract permits it.

The first rejected submission or explicit `fail()` is sticky. Later submissions are safe no-ops and return null, zero,
or invalid handles as appropriate. The framework discards the partial Plan and reports the first actionable reason.

Use `fail()` for an author condition that prevents a valid Plan but cannot be expressed by an operation:

```cpp
if (!supported_revision(plan.target().image_profile)) {
    plan.fail("The selected executable revision has no reviewed patch locations", "Select target locations");
    return;
}
```

### Requirements

Requirements validate unchanged native facts and optionally expose a resolved address for later use.

| Method | Result | Claim and constraints |
| --- | --- | --- |
| `require(DataLocation<T, Count>)` | Aligned `const T*` | Read over `sizeof(T) * Count` bytes |
| `require_mutable(DataLocation<T, Count>)` | Aligned `T*` | Write for the installed lifetime |
| `require(FunctionLocation<Call>)` | Function pointer or callable | Executable entry and call description |
| `require(CodeLocation, size)` | `std::uintptr_t` | Read over a nonempty executable range |
| `require(VtableLocation, byte_size)` | `std::uintptr_t` | Read over a nonempty data range |
| `require_at(rva, size, evidence)` | `std::uintptr_t` | Compact untyped code requirement |

Data requirements enforce native alignment. A rejected requirement returns null, zero, or an invalid explicit callable
and fails the Plan. Evidence-less function resolution still checks the selected image, architecture, executable bounds,
and supported call description.

Returned addresses and callables are for later lifecycle phases or installed runtime work. Do not call game functions
or mutate game data during the Plan callback. `require_mutable` is a declaration of future access: it neither changes
page protection nor synchronizes with the game's own readers and writers. The complete range must already be writable,
and author code may use it only after successful installation.

Use a requirement when the patch relies on native state that another operation does not already cover:

```cpp
const auto* state = plan.require(fc::DataLocation<std::uint32_t>{
    .rva = fc::Rva{0x00102030},
    .name = "GameStateFlags",
    .evidence = fc::expect(std::uint32_t{0}),
});
```

### Writes

`write(location, replacement)` and `write_at(rva, replacement, evidence)` select an encoding from the replacement type.

| Replacement | Destination | Effect |
| --- | --- | --- |
| Trivially copyable value | Data or code | Copies the value's complete object representation |
| Contiguous range of trivially copyable values | Data or code | Copies the complete nonempty range |
| Image location | Data | Writes the selected image address as a native pointer |
| `DataAddress` | Data | Writes the resolved framework allocation address as a native pointer |
| Function pointer owned by the plugin | Data | Writes the function address as a native pointer |
| `rel32(target)` | Data or code | Writes a four-byte signed relative displacement |
| `call_to(target)` | Code | Writes a new five-byte direct near call |
| `jump_to(target)` | Code | Writes a new five-byte direct near jump |

Valid address targets are semantic image locations, symbolic `DataAddress` values, and appropriate native functions in
the submitting plugin. A referenced image location with evidence causes the SDK to submit the matching requirement
before the write; its evidence is never silently discarded.

`call_to` and `jump_to` accept executable image locations or plugin functions. A symbolic data address is valid for a
pointer or `rel32` write but is not an executable branch target.

Typed values need no manual byte conversion:

```cpp
plan.write(
    fc::DataLocation<std::uint32_t>{
        .rva = fc::Rva{0x00102030},
        .name = "MaximumPlayers",
        .evidence = fc::expect(std::uint32_t{32}),
    },
    std::uint32_t{64});
```

Literal instruction bytes use the same operation:

```cpp
constexpr std::array replacement{
    std::byte{0xeb},
    std::byte{0x05},
};

plan.write(
    fc::CodeLocation{
        .rva = fc::Rva{0x00123456},
        .name = "AcceptCorrectedState",
        .evidence = fc::exact_bytes({std::byte{0x74}, std::byte{0x05}}),
    },
    replacement);
```

`call_to` and `jump_to` insert a new branch; they do not require an existing branch and do not return an original
target. Reachability is checked after symbolic addresses and private relays are resolved. Use literal bytes for a short
branch or another reviewed instruction sequence; the SDK does not expose a general assembler.

### NOP ranges

```cpp
plan.nop(code_location, byte_size);
plan.nop_at(rva, byte_size, evidence);
```

Both forms require a nonempty executable range and replace the complete range with architecture-appropriate NOP bytes.
The location evidence normally covers the original instructions being removed.

### Existing branch redirects

Redirects preserve and retarget one existing supported direct call or unconditional direct jump:

| Placement | Structured form | Compact form |
| --- | --- | --- |
| Direct call | `redirect_call(CallLocation<Function>, replacement)` | `redirect_call_at(rva, replacement, evidence)` |
| Direct jump | `redirect_jump(CodeLocation, replacement)` | `redirect_jump_at(rva, replacement, evidence)` |

The replacement is either a matching native function in the plugin or a `FunctionLocation<Function>` in the selected
image. Each overload returns the decoded original target as the same function-pointer type:

```cpp
using Update = void (*)() noexcept;

void replacement_update() noexcept;

const fc::FunctionLocation<Update> original_update{
    .rva = fc::Rva{0x00103020},
    .name = "OriginalUpdate",
};

const fc::CallLocation<Update> site{
    .rva = fc::Rva{0x00104050},
    .name = "UpdateCall",
    .evidence = fc::direct_call_to(original_update),
};

Update original = plan.redirect_call(site, &replacement_update);
```

The returned target may be retained for later use but must not be called during the Plan callback. The framework
revalidates the branch form and original target before Prepare and Commit. Conditional branches use a reviewed byte
write or a new `jump_to` instruction instead.

### Symbolic native data

```cpp
DataHandle<T> allocate_data<T>(std::size_t count = 1, std::string_view name = {});
DataHandle<T> allocate_data<T>(std::span<const T> initial_values, std::string_view name = {});
```

The count form produces zero-initialized storage. The span form copies a complete nonempty initial sequence. `T` must
satisfy `NativeData`; the SDK does not construct, destroy, or transfer ownership of nontrivial plugin objects.

`DataHandle<T>` belongs to one Plan and may be retained for `PrepareContext::resolve()`. Its address helpers are for use
while that Plan callback is running:

| Method | Symbolic address |
| --- | --- |
| `base()` | First byte of the allocation |
| `element(index)` | One in-range element |
| `byte_offset(offset)` | One in-range byte offset |
| `end()` | Explicit one-past address |

`DataAddress` is opaque. It has no arithmetic, absolute-address conversion, or direct resolution method. Invalid
offsets fail the Plan, and a handle cannot be used by another patch.

This example reserves a larger table and plans a pointer replacement without knowing the final native address:

```cpp
const auto table = plan.allocate_data<TableEntry>(entry_count, "Expanded mission table");
plan.write(table_pointer_location, table.base());
plan.write(table_end_location, table.end());
```

The framework allocates and initializes storage only after common validation and conflicts succeed. It resolves every
symbolic pointer, displacement, and branch before calling plugin Prepare. The SDK begins `T`'s lifetime first;
`PrepareContext::resolve(handle)` then returns the same allocation as `std::span<T>`. Committed native storage has
process lifetime.

### Optional interface bindings

`bind<Interface>(provider_patch, callback)` plans one optional connection to a copied patch interface. The callback is
either a nonthrowing `void(Interface) noexcept` callable or a matching member function:

```cpp
plan.bind<CounterApi>("CounterProvider", *this, &Handler::connect_counter);
```

Binding does not select, require, or order the provider and creates no memory claim. It may connect before or after the
consumer activates, including after a late image, so the consumer owns synchronization with its runtime paths. Use
`depends_on` plus `PrepareContext::find_interface()` when the interface is required. See
[Patch interfaces](#patch-interfaces) for contract and callback details.

### Hook ownership

| Placement | Structured form | Compact form |
| --- | --- | --- |
| Function entry | `hook(FunctionLocation<Call>, callback)` | `hook_entry_at<Call>()` |
| Direct call site | `hook(CallLocation<Call>, callback)` | `hook_call_at<Call>()` |
| Instruction boundary | `hook(CodeLocation, callback)` | `hook_code_at()` |

Function-entry ownership changes every invocation of the function. Direct-call-site ownership changes only the one
reviewed call instruction. Instruction ownership is the fallback for a reviewed instruction boundary that has no
complete supported logical function signature.

For logical `Return(Args...)`, a typed owner callback is:

```cpp
Return callback(fc::Original<Call> original, Args... arguments) noexcept;
```

The callback may call `original(arguments...)`, replace behavior, or combine both. It must match the declared logical
arguments and result and must be `noexcept`:

```cpp
using SendPacket = int (*)(Peer*, Packet*) noexcept;

plan.hook(send_location,
          [this](fc::Original<SendPacket> original, Peer* peer, Packet* packet) noexcept -> int {
              if (should_send_directly(*packet)) {
                  return send_directly(*peer, *packet);
              }
              return original(peer, packet);
          });
```

An instruction owner is `void(fc::CpuContext&) noexcept`. It may change the saved CPU state or resume address but
receives no typed `Original`.

`Original<Call>` is copyable and unbound during the Plan callback. It becomes callable before the Prepare method and
remains valid while the patch is installed. Check it only when code may run before successful installation; calling an
unbound handle violates its precondition.

One patch may contribute only once to a physical hook site, as either the owner or one observer. Compatible observers
share the physical hook with one owner. Multiple owners conflict.

### Hook observation

| Placement | Structured form | Compact form |
| --- | --- | --- |
| Function entry | `observe(FunctionLocation<Call>, ...)` | `observe_entry_at<Call>()` |
| Direct call site | `observe(CallLocation<Call>, ...)` | `observe_call_at<Call>()` |
| Instruction boundary | `observe(CodeLocation, ...)` | `observe_code_at()` |

Wrap each callback with `fc::before()` or `fc::after()`. For `Return(Args...)`:

- before: `void(Args...) noexcept`;
- after with non-void result: `void(Args..., Return) noexcept`; and
- after with void result: `void(Args...) noexcept`.

A paired observer can name a trivial invocation-local state type:

```cpp
plan.observe<State>(location, fc::before(before_callback), fc::after(after_callback));
```

For a typed call, the before callback receives `(Args..., State&)`. The after callback receives
`(Args..., Return, const State&)`, or `(Args..., const State&)` for a void result. State alignment must not exceed 16
bytes. An instruction observer receives `const fc::CpuContext&` and cannot modify execution; paired instruction
observers receive the mutable or const state after that context.

Observers cannot replace arguments, results, or control flow. FusionCutter automatically preserves Win32 and Winsock
ambient error state around shared-hook observers.

One site supports at most 16 observers and 1024 total aligned state bytes. Paired state must be trivial and aligned to
at most 16 bytes. Nested and concurrent invocations receive separate state.

Dispatch order is deterministic:

1. before observers in case-insensitive patch ID order;
2. the owner, or the original behavior when there is no owner; and
3. after observers in reverse patch ID order.

Relationships and registration order do not change callback order. Hook and observation callbacks may be nested or run
concurrently on game threads. They must not allocate, log, perform I/O, block, or do unbounded work.

### CPU context

`fc::CpuContext` aliases the architecture-specific stable `FC_CpuContext`. It exposes general registers, flags, SIMD
registers, the captured stack pointer, an effective `resume_esp` or `resume_rsp`, and the resume instruction pointer.
Writing the captured stack pointer has no effect; use the corresponding `resume_*` field to change resumption.

`fc::SimdRegister` aliases `FC_SimdRegister`, whose views include `u8`, `u16`, `u32`, `u64`, `f32`, and `f64` arrays.

An instruction observer receives `const CpuContext&`; only the one instruction owner receives a mutable context.
Instruction observers and the owner cannot change the surrounding function's ambient Win32 or Winsock error state.

### Claims, conflicts, and commit

Every accepted operation contributes private Read or Write claims over the selected image:

| Operation | Claim |
| --- | --- |
| Read-only requirement | Read over its explicit extent and evidence |
| Mutable data requirement | Write over its complete declared extent |
| Literal or address write | Write over its destination extent |
| NOP, redirect, or hook | Write over the complete native overwrite range |
| Symbolic allocation | No game-image claim; references claim their destinations |

The conflict rules are:

- read/read overlap is allowed;
- writes from different patches cannot overlap;
- one patch cannot write another patch's read dependency;
- adjacent half-open ranges are allowed;
- read/write overlap within one patch is allowed; and
- separate overlapping writes within one Plan are invalid.

Compatible participants at one shared-hook site form one aggregate write claim. An unrelated overlapping write conflicts
with every participant at that site.

After common validation, the framework allocates approved private native resources, runs the Prepare phase, and
revalidates the facts that could have changed. Commit alone changes protection, writes memory, installs hooks, restores
protection, and flushes the instruction cache. A partial failure rolls back completed work in reverse order. The
directly failing patch and required consumers fail or skip according to their relationships and failure policies;
independent patches continue unless an applicable `Fatal` policy stops initialization.

## Patch interfaces

Use an interface for a narrow cross-plugin service, policy, snapshot, or observation attachment that shared hooks do
not express. Patches in the same plugin may use ordinary private C++.

### Contract

```cpp
struct CounterApi {
    static constexpr std::string_view id = "CounterApi";
    fc::InterfaceFunction<std::int32_t() noexcept> read;
};

static_assert(fc::InterfaceContract<CounterApi>);
```

An interface contract is standard-layout, trivially copyable, at most 512 bytes, and aligned no more strictly than
`std::max_align_t`. It contains fixed-layout values, opaque non-owning contexts, and `FC_CALL` `noexcept` functions or
the SDK wrappers below. It must not contain STL objects, references, plugin classes, ownership-bearing pointers, or a
cross-DLL deletion contract.

Changing a contract layout requires a new interface ID whose spelling makes the incompatible revision clear.

### Callable wrappers

| Type or helper | Use |
| --- | --- |
| `InterfaceFunction<Result(Args...) noexcept>` | Ordinary service or policy call |
| `interface_function<&Type::method>(object)` | Builds an `InterfaceFunction` from a nonthrowing member method |
| `Observation<void(Args...) noexcept>` | Transparent retained observation callback |
| `observation<&Type::method>(consumer)` | Builds an observation bridge preserving ambient error state |

Both wrapper types are default-empty and convert to `bool`. Calling an empty value violates its precondition.

### Providing an interface

Implement `void query_interface(fc::InterfaceQuery&) noexcept` and submit each supported contract:

```cpp
void Handler::query_interface(fc::InterfaceQuery& query) noexcept {
    query.provide(CounterApi{fc::interface_function<&Handler::read>(*this)});
}
```

The query copies the first matching exact ID and layout. Interface availability is fixed when the provider activates.

### Required lookup

Use a patch dependency and look up the active provider during the Prepare phase:

```cpp
auto interface = context.find_interface<CounterApi>("CounterProvider");
```

The result is `std::optional<CounterApi>`. The dependency orders activation but does not guarantee that the provider
implements the requested contract, so the consumer must handle a missing result.

### Optional binding

Plan a one-time optional connection with:

```cpp
plan.bind<CounterApi>("CounterProvider", *this, &Handler::connect_counter);
```

or a nonthrowing `void(CounterApi) noexcept` callable. Binding does not select, require, or order the provider. The
connection runs at most once if the provider becomes available before or after the consumer, including after a late
image. A late connection may overlap the consumer's runtime paths, so the consumer owns publication synchronization.

## Logging, status, and traces

### Logger

`Logger` is already bound to the owning plugin and patch. It is copyable and may be retained for installed runtime
work.

| Method | Purpose |
| --- | --- |
| `enabled(level)` | Checks whether a level is active |
| `write(level, message)` | Submits already-produced text |
| `error(format, args...)` | Failed operation |
| `warning(format, args...)` | Unexpected or degraded behavior |
| `info(format, args...)` | Useful normal lifecycle or user-facing event |
| `debug(format, args...)` | Development detail, decisions, and resolved facts |

Formatted methods check the level before formatting. Repetitive or high-volume runtime records belong in a trace
channel rather than ordinary logs.

### Status writer

`StatusWriter::add(label, value)` accepts:

- UTF-8 text;
- signed and unsigned integral values except `bool`;
- `double`; and
- `bool`.

It returns false when the field is invalid or the patch's bounded live section is full. Status callbacks should read a
small current snapshot; they must not call the game or network, perform I/O, wait, or append history.

### Trace channels

Create a channel during the Prepare phase:

```cpp
auto trace = context.create_trace({
    .name = "PacketEvents",
    .capacity = 256,
    .max_record_size = sizeof(PacketRecord),
});
```

`create_trace()` returns `std::expected<TraceChannel, Error>`. A configuration-disabled channel is a successful inert
handle. A rejected definition returns an error.

`TraceDefinition` contains `name`, `capacity`, `max_record_size`, and a schema `version` that defaults to 1.
`TraceHealth` contains monotonic `accepted`, `written`, and `dropped` counters plus `file_limit_reached` and
`output_failed` flags.

`TraceChannel` is move-only and provides:

| Method | Purpose |
| --- | --- |
| `enabled()` | Whether the channel can currently accept output |
| `try_write(record)` | Nonblocking typed write of a standard-layout, trivially copyable record |
| `try_write(bytes)` | Nonblocking write of a nonempty byte payload |
| `health()` | Accepted, written, dropped, cap, and output-failure snapshot |

Capacity counts pending records. `max_record_size` counts patch payload bytes and cannot exceed 60 KiB. A write is a
bounded copy with no allocation, formatting, file I/O, mutex, or waiting. Trace record schemas and decoders are owned
by the patch.

## Public testing API

Include `FusionCutter/Testing.hpp` and link `FusionCutter::Testing` only in test executables.

### Scenario

```cpp
fc::test::Scenario scenario(layout, role);
scenario.add_plugin(plugin_path);
scenario.add_image(image, image_profile, file_path);
scenario.add_image(image, image_profile, loaded_image_bytes);
scenario.use_config(config_directory);
auto result = scenario.validate();
```

`Scenario` uses production admission, configuration, support selection, relationships, the Create and Plan phases,
evidence, claims, conflicts, and common validation. It does not run the Prepare, Commit, Activate, or Update phases or
status, and it never mutates the input image.

File images are privately mapped into loaded-image layout. Byte spans must already represent loaded-image layout and
remain valid until `validate()` returns. Configuration files are copied to a temporary workspace before parsing or
generation.

### Scenario results

| Type | Purpose |
| --- | --- |
| `PluginResult` | Candidate path, trusted ID when available, `admitted` flag, and rejection reason |
| `PatchResult` | Patch state, shallow failure facts, validated operations, and claims |
| `Operation` | Operation index, kind, optional image/RVA, byte size, and evidence presence |
| `Claim` | Image range, Read/Write access, and originating operation |
| `ScenarioResult` | Owns plugin and patch results and supports ID lookup |

The result records expose:

| Record | Fields |
| --- | --- |
| `PluginResult` | `path`, `id`, `admitted`, `reason` |
| `PatchResult` | `id`, `state`, failure details, `operations`, `claims` |
| `Operation` | `operation_index`, `kind`, optional `image` and `rva`, `byte_size`, `has_evidence` |
| `Claim` | `image`, `rva`, `byte_size`, `access`, `operation_index` |

`ScenarioResult::plugins()` and `patches()` return borrowed spans. The two `find_*` methods return pointers into the
result or `nullptr`.

`PatchState` values are `Disabled`, `NotApplicable`, `WaitingForImage`, `Ready`, `Skipped`, and `Failed`.
`PatchPhase` values are `Selection`, `Settings`, `Create`, `Plan`, and `Validation`. `OperationKind` values are
`Require`, `Write`, `Nop`, `Redirect`, `AllocateData`, `BindInterface`, `Hook`, and `Observe`.
`ClaimAccess` values are `Read` and `Write`.

A top-level `fc::Error` means the scenario could not run. Plugin rejection and patch failure are ordinary entries in a
successful `ScenarioResult`.

## C ABI reference

The C++ SDK lowers to the stable generation-1 C ABI. Most patch authors should not use these records directly.

### `FusionCutter/Abi.h`

Defines `FC_CALL`, `FC_EXTERN_C`, `FC_NOEXCEPT`, `FC_Bool`, `FC_StringView`, `FC_ByteView`, target layout/role/image
types, architecture values, and failure policies. Public ABI records use pack 8 without changing the includer's outer
packing state.

### `FusionCutter/PluginApi.h`

Defines:

- plugin query and registration tables: `FC_PluginApi`, `FC_HostApi`, `FC_RegistrySink`, `FC_ErrorSink`;
- plugin catalog records: `FC_PluginDefinition`, `FC_PatchDefinition`, `FC_SupportDefinition`,
  `FC_GroupDefinition`, and `FC_CategoryDefinition`;
- settings records: `FC_SettingDefinition`, `FC_SettingValue`, and `FC_SettingsView`;
- lifecycle contexts and `FC_PatchCallbacks`;
- Plan locations, evidence, address targets, operation requests, and `FC_PlanSink`;
- normalized native-call records and architecture register values;
- shared-hook request, builder, snapshot, owner, observer, and CPU context records;
- interface binding requests;
- logging, status, and trace tables; and
- `FusionCutter_QueryPlugin()` with `FC_PLUGIN_ABI_GENERATION` and `FC_SDK_REVISION`.

Pointer/count views are callback-scoped unless the record states otherwise. The framework copies accepted metadata and
Plan payloads during the call. C adapters must obey the exact contracts governing record sizes, inactive fields,
callbacks, lifetimes, and exception containment in the header and ABI specification.

### `FusionCutter/CoreApi.h`

Defines the loader-facing `FusionCutter_QueryCore()` contract, `FC_CoreApi`, initialization results, loader kinds,
DirectInput chain results, and startup records. Plugins do not query or call this API.

## Important limits

| Resource | Generation-1 limit |
| --- | ---: |
| External plugin candidates | 128 |
| Global patch and group definitions | 4096 |
| Copied metadata per plugin | 2 MiB |
| Copied Plan payload per patch | 2 MiB |
| Copied Plan payload per validation run | 64 MiB |
| Callback error text | 1024 UTF-8 bytes total |
| Interface contract | 512 bytes |
| Shared-hook observers per site | 16 |
| Shared-hook state per site | 1024 aligned bytes |
| Trace channel storage across the process | 8 MiB |
| Trace payload | 60 KiB maximum per record |
| Live status section | 4096 rendered bytes |

These limits are capacities, not allocation targets. Prefer focused definitions and bounded runtime behavior rather
than designing to consume the maximum.
