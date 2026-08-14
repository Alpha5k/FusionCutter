# Patch Author Reference

This is the complete lookup for Fusion Cutter's patch-facing framework. Start with [Writing a Patch](patches.md) if you are new to the model; return here for exact names, signatures, and constraints.

## Quick lookup

| I want to... | Use | See |
|---|---|---|
| Register a patch and select its targets | `fc_patch()`, `PatchDefinition`, `make_patch_variant()` | [Patch identity and registration](#patch-identity-and-registration) |
| Choose startup or runtime behavior | `Patch`, `RuntimePatch`, `RuntimeOnlyPatch` | [Patch classes and lifecycle](#patch-classes-and-lifecycle) |
| Validate, write, hook, or redirect native code | `PatchPlan`, `BytePattern`, hook and redirect methods | [Memory and hook tools](#memory-and-hook-tools) |
| Allocate data or access recognized game state | `allocate_data()`, `ImageContext`, native-field helpers | [Data allocation](#data-allocation), [Target access](#target-access) |
| Declare settings or read an environment override | `setting()`, `choice()`, groups, environment helpers | [Settings](#settings), [Environment values](#environment-values) |
| Bridge callbacks, receive updates, or publish status | `PatchInstanceSlot<T>`, `Updatable`, `StatusContributor` | [Patch classes and lifecycle](#patch-classes-and-lifecycle), [Reporting](#reporting) |
| Report diagnostics or actionable failures | `logging::*`, `OutcomeReason` | [Reporting](#reporting), [Outcomes](#outcomes-and-failure-reporting) |
| Publish a high-volume ETL diagnostic channel | `diagnostics::EtlChannel` | [ETL diagnostics](#etl-diagnostics) |
| Build, test, and verify supported binaries | CMake, CTest, `FusionCutter-Verify.exe` | [Build and verification tools](#build-and-verification-tools) |

The primary include for a patch is:

```cpp
#include <FusionCutter/patch.hpp>
```

It exposes the patch, plan, settings, target, reporting, and outcome interfaces documented here.

## Patch identity and registration

### Build manifest

Every patch has one `patch.cmake` below `src/patches`. CMake discovers these files recursively.

```cmake
fc_patch(
    ID ExampleFix
    DEFINITION fusioncutter::patches::example_fix::definition
    ARCHITECTURES X86 X64
    ROLES CLIENT SERVER
    SOURCES
        patch.cpp
        example_fix.cpp
)
```

All fields are required:

| Field | Accepted values and purpose |
|---|---|
| `ID` | Unique stable patch ID. Used by INI selection, dependencies, status, and logs. |
| `DEFINITION` | Fully qualified function returning `PatchDefinition`. |
| `ARCHITECTURES` | One or both of `X86`, `X64`; controls which artifacts compile the patch. |
| `ROLES` | One or both of `CLIENT`, `SERVER`; controls which artifacts compile the patch. |
| `SOURCES` | Project-owned source files always compiled for this patch. Paths are relative to the manifest. |

The manifest ID, not the folder name or user-facing name, is the patch's identity.

### Conditional sources

Use `fc_patch_sources()` when source files truly differ by architecture or role:

```cmake
fc_patch_sources(
    PATCH ExampleFix
    ARCHITECTURES X64
    ROLES CLIENT
    SOURCES
        classic/example_fix.cpp
)
```

`PATCH`, `ARCHITECTURES`, `ROLES`, and `SOURCES` are required. Runtime layout selection still belongs in variants; this command only keeps incompatible implementation files out of an artifact.

### `PatchDefinition`

The definition function returns fields in this designated-initializer order:

```cpp
struct PatchDefinition {
    std::string_view name;
    bool enabled;
    bool configurable;
    PresentationCategory category;
    std::string_view description;
    SettingsDefinition settings;
    std::span<const PatchRelationship> depends_on;
    std::span<const PatchRelationship> includes;
    std::span<const PatchVariant> variants;
};
```

| Field | Meaning |
|---|---|
| `name` | User-facing name in status and generated configuration comments. |
| `enabled` | Default selected state. |
| `configurable` | Whether the user can select/configure the patch. A nonconfigurable patch cannot declare settings. |
| `category` | Presentation grouping and order. |
| `description` | Optional user-facing explanation; empty means no generated comment. |
| `settings` | Default typed schema for the patch's variants; default construction means no dedicated settings. |
| `depends_on` | Patch references that must install successfully first. |
| `includes` | Companion patch references automatically selected with this patch; explicit disable wins. |
| `variants` | Supported layout, role, image, timing, implementation, and failure-policy combinations. |

Current shared categories from `<FusionCutter/categories.hpp>` are:

| Constant | Display name | Order |
|---|---|---|
| `categories::GeneralFixes` | General Fixes | `50` |
| `categories::Limits` | Limits | `100` |
| `categories::Multiplayer` | Multiplayer | `200` |
| `categories::Networking` | Networking | `250` |
| `categories::Server` | Server | `300` |
| `categories::Diagnostics` | Diagnostics | `350` |

`PresentationCategory` has `name` and integer `order` fields.

`PatchId` is `std::string_view` and must refer to stable catalog-owned text.

### `PatchRelationship`

Relationships apply to every variant unless they name a layout, role, or both:

```cpp
PatchRelationship(PatchId patch_id)
PatchRelationship(PatchId patch_id, HostRole role)
PatchRelationship(PatchId patch_id, TargetLayout layout)
PatchRelationship(PatchId patch_id, TargetLayout layout, HostRole role)
```

The field containing the relationship supplies its meaning: `depends_on` makes it required, while `includes` selects
it as an optional companion. Specified scopes must all match the active variant.

## Patch classes and lifecycle

### Patch classes

| Base class | Required work | Use when |
|---|---|---|
| `Patch` | `void build_plan(PatchPlan&)` | The patch describes one-time native validation and memory work. |
| `RuntimePatch : Patch` | `build_plan()`; runtime methods are optional overrides | The patch also keeps callbacks, workers, sockets, or other runtime state. |
| `RuntimeOnlyPatch` | Runtime methods are optional overrides | Runtime behavior needs no game-memory plan. |

`Patch` and `RuntimeOnlyPatch` have virtual destructors. A patch without settings must be constructible from `const TargetContext&`. A patch with settings must be constructible from `Settings, const TargetContext&`.

### Runtime lifecycle

Both runtime base classes provide:

```cpp
virtual std::expected<void, OutcomeReason> prepare_runtime();
virtual void enable_runtime() noexcept;
virtual void disable_runtime() noexcept;
```

Their defaults succeed or do nothing. The core uses this order:

1. Construct the patch.
2. Build, validate, and reserve all selected plans.
3. Call `prepare_runtime()` while callbacks remain inactive.
4. Commit the plan.
5. Call `enable_runtime()` to publish prepared state.
6. Call `disable_runtime()` before undoing mutations during rollback or shutdown.

`prepare_runtime()` is the fallible stage. `enable_runtime()` and `disable_runtime()` are `noexcept` gates, not places for resource acquisition or blocking teardown.

### Optional runtime capabilities

| Base | Required method | Contract |
|---|---|---|
| `Updatable` | `void update() noexcept` | Bounded, nonblocking work driven by the approved host pump. It is not a scheduler. |
| `StatusContributor` | `void write_status(StatusSection&) const noexcept` | Adds a bounded snapshot to the shared status output. |

A runtime class may derive from either or both. The framework detects these capabilities from the object; no definition flags are needed.

### Callback bridge

`PatchInstanceSlot<T>` connects one active patch object to static/native callbacks:

| Method | Result |
|---|---|
| `publish(T& instance) noexcept` | Release-publishes the active instance. |
| `read() const noexcept -> T*` | Acquire-loads the active pointer, or `nullptr`. |
| `clear(T& instance) noexcept` | Clears the slot only if it still contains that instance. |

It is a single-instance bridge, not a registry or subscriber system. Callbacks must handle `nullptr` safely.

## Variants

Create descriptors with:

```cpp
make_patch_variant<PatchType, TargetLayout, HostRole, Settings = NoSettings>(
    TargetImage image,
    ImageTiming image_timing = ImageTiming::Startup,
    StartupFailurePolicy failure_policy = StartupFailurePolicy::Local)
```

When a startup image is required, omit the default timing rather than spelling it out:

```cpp
make_patch_variant<PatchType, TargetLayout, HostRole, Settings>(
    TargetImage::Game, StartupFailurePolicy::StartupRequired)
```

The patch definition's schema is the default for every variant. When one role or target exposes different settings,
pass that variant's schema after the image:

```cpp
make_patch_variant<ServerTransport, TargetLayout::GOGRetail, HostRole::Server, DirectTransportSettings>(
    TargetImage::Game,
    server_settings(),
    StartupFailurePolicy::StartupRequired)
```

`DirectTransportSettings` is the compile-time object type used by the factory and patch constructor.
`server_settings()` is the schema override that defines its configuration metadata and parsing. The type remains an
explicit template argument because `SettingsDefinition` is type-erased. This changes settings only for that exact
variant; the patch keeps one stable identity and one toggle. Variants without dedicated settings omit both and use
`NoSettings`.

Collect them in `PatchVariants`:

```cpp
const PatchVariants kVariants{
    make_patch_variant<ExampleFix, TargetLayout::SteamRetail, HostRole::Client>(TargetImage::Game),
    make_patch_variant<ExampleFix, TargetLayout::GOGRetail, HostRole::Client>(TargetImage::Game),
};
```

Variants for other architectures or roles are removed at compile time. Do not reproduce this dispatch with compiler architecture macros in patch code.

### Target layouts

| `TargetLayout` | Architecture | User-facing target |
|---|---|---|
| `SteamRetail` | x86 | Steam retail |
| `GOGRetail` | x86 | GOG retail |
| `ModTools` | x86 | Mod Tools |
| `Aspyr` | x64 | Classic Collection |

`target_architecture(layout)` returns the layout's `Architecture::X86` or `Architecture::X64`.

### Roles, images, timing, and failure policy

| Enum | Values | Meaning |
|---|---|---|
| `HostRole` | `Client`, `Server` | Host process role. |
| `TargetImage` | `Game`, `Bootstrap`, `GalaxyPeer` | Physical image owned by the plan. |
| `ImageTiming` | `Startup`, `OneShotLate` | Image is present at startup or may be handled once when it appears later. |
| `StartupFailurePolicy` | `Local`, `StartupRequired` | Failure remains patch-local or promotes initialization to fatal for that applicable variant. |

One plan owns one physical image. A feature spanning multiple images uses related patches with explicit relationships. `OneShotLate` cannot be `StartupRequired` because the image may not exist during startup.

## Memory and hook tools

All operations below are methods of the core-owned `PatchPlan&` passed to `build_plan()`. RVAs are relative to the variant's selected image. Each operation requires a nonempty, human-readable operation name.

Patch code describes work only. The core owns preimage validation, range ownership, conflict detection, memory protection, instruction-cache handling, preparation, commit, and rollback.

### Byte patterns

| API | Use |
|---|---|
| `byte_array<0x90, ...>()` | Create a `consteval std::array<std::byte, N>` without verbose `std::byte` entries. |
| `BytePattern::exact(bytes)` | Require every bit to match. |
| `BytePattern::masked(bytes, mask)` | Compare only mask bits: `(actual & mask) == (expected & mask)`. |
| `NativeSite<N>{rva, expected, mask}` | Keep an RVA and owned expected bytes together. Omit `mask` for an exact site; `pattern()` returns the matching pattern. |
| `embed_value<Offset>(bytes, value)` | Insert any trivially copyable fixed-width value with compile-time bounds checking. |
| `embed_image_address<Offset>(bytes, image, rva)` | Insert the architecture-sized address resolved from an image RVA. |
| `embed_relative_displacement<Offset, Type>(bytes, base_rva, target_rva)` | Insert a signed displacement of `Type` from the explicitly supplied native base RVA. `Type` defaults to `std::int32_t`. |

A pattern must contain at least one byte. A nonempty mask must have the same size as the byte span and constrain at least one bit. The plan copies pattern storage, so local arrays do not need to outlive `build_plan()`.

### Writes and validation

| Method | Use and requirements |
|---|---|
| `checked_write(operation, rva, BytePattern expected, span<byte> replacement)` | Replace bytes after validating the preimage. Expected and replacement sizes must match. |
| `checked_write(operation, rva, const T& expected, const T& replacement)` | Write one trivially copyable typed value using exact object bytes. |
| `checked_write(operation, rva, BytePattern expected, PatchAddress replacement)` | Write a resolved native pointer. The expected size must equal the target pointer size. |
| `checked_write(operation, rva, const T& expected, PatchAddress replacement)` | Pointer write with a typed expected native address or value. |
| `nop(operation, rva, BytePattern expected)` | Replace the expected span with `0x90` bytes. |
| `require_bytes(operation, rva, BytePattern expected)` | Prove an unchanged native dependency and claim a read range. |
| `require_bytes(operation, rva, const T& expected)` | Prove one trivially copyable typed dependency using exact object bytes. |

Read/read overlap is allowed. A mutation conflicting with another selected operation's mutation or required read causes a conflict. Do not duplicate validation with `require_bytes()` when an operation's own preimage already proves the fact.

### Hooks

| Method | Use and requirements |
|---|---|
| `inline_hook(operation, rva, expected, destination)` | Hook a whole function without retaining its original. The preimage covers all instructions SafetyHook will replace. |
| `inline_hook_with_original(operation, rva, expected, destination)` | Hook a whole function and return its typed trampoline as `OriginalFunction<Function>`. |
| `mid_hook(operation, rva, expected, MidHookCallback)` | Hook a reviewed instruction boundary and receive saved register state. The preimage covers the replaced instruction span. |

Hooks are created disabled during preparation and enabled by the core during commit. Patches do not receive or toggle underlying SafetyHook objects.

`MidHookCallback` is:

```cpp
void (*)(MidHookContext& context) noexcept
```

`SimdRegister` exposes the same 16 bytes through `u8[16]`, `u16[8]`, `u32[4]`, `u64[2]`, `f32[4]`, or `f64[2]`.

| Architecture | `MidHookContext` fields |
|---|---|
| x86 | `xmm0`-`xmm7`; `eflags`, `edi`, `esi`, `edx`, `ecx`, `ebx`, `eax`, `ebp`, `esp`, `trampoline_esp`, `eip` |
| x64 | `xmm0`-`xmm15`; `rflags`, `r15`-`r8`, `rdi`, `rsi`, `rdx`, `rcx`, `rbx`, `rax`, `rbp`, `rsp`, `trampoline_rsp`, `rip` |

The callback may adjust saved state for that reviewed boundary. It remains `noexcept`, bounded, and free of blocking, file I/O, recurring discovery, and avoidable allocation.

### Redirects

| Method | Use |
|---|---|
| `redirect(operation, rva, expected, RedirectKind, PatchAddress)` | Generic `Call` or `Jump` to an absolute, image-relative, or otherwise supported address. |
| `redirect(operation, rva, expected, RedirectKind, Function)` | Function-pointer destination overload. |
| `redirect_with_original(operation, rva, expected, RedirectKind, destination)` | Generic call or jump redirect that retains the exact direct branch's original target. |
| `redirect_call(operation, rva, expected, destination)` | Convenience wrapper for `RedirectKind::Call`. |
| `redirect_call_with_original(operation, rva, expected, destination)` | Redirect an exact direct `E8` call and retain its decoded original target. |
| `redirect_jump(operation, rva, expected, destination)` | Convenience wrapper for `RedirectKind::Jump`. |
| `redirect_jump_with_original(operation, rva, expected, destination)` | Redirect an exact direct `E9` jump and retain its decoded original target. |

`RedirectKind` values are `Call` and `Jump`. Redirect preimages contain at least five replaceable bytes. The core writes `E8` or `E9`, calculates the displacement, and NOP-fills any remaining claimed bytes. It may create a near relay for an out-of-range x64 destination; an out-of-range x86 redirect fails.

The `*_with_original()` redirect methods require the first five bytes to be fully constrained and to begin with the
matching direct-call (`E8`) or direct-jump (`E9`) opcode.

### Original functions

`inline_hook_with_original()` and the redirect `*_with_original()` methods return `OriginalFunction<Function>`:

| Method | Result |
|---|---|
| `get() const noexcept` | Typed original/trampoline after successful commit, otherwise `nullptr`. |
| `explicit operator bool() const noexcept` | Whether `get()` is non-null. |

Use an exact function-pointer type, including the reviewed calling convention and parameters. Do not toggle a hook merely to call its original.

### Data allocation

```cpp
allocate_data<T>(
    operation,
    count,
    std::span<const T> initial_values = {},
    std::optional<AllocationProximity> proximity = std::nullopt)
```

`T` must be trivially copyable and non-const/non-volatile. The core allocates aligned read/write memory, zero-initializes it, copies any initial prefix, and owns it for the patch transaction/process lifetime.

`AllocationProximity{anchor_rva, maximum_distance}` requests data near an image RVA. Its default maximum distance is
`0x7FFF'FFFF`.

`AllocatedData<T>` is the returned symbolic handle:

| Method | Result |
|---|---|
| `size()` | Element count. |
| `explicit operator bool()` | Whether the handle owns a symbolic slot, not whether preparation has run. |
| `base()` | Symbolic base `PatchAddress`. |
| `element(index)` | Symbolic address of an in-range element; otherwise invalid. |
| `byte_offset(offset)` | Symbolic byte-offset address; plan validation rejects an out-of-range use. |
| `data()` | Runtime `T*` after successful preparation, or `nullptr` before allocation/release. Never dereference it in `build_plan()`. |

Allocation addresses may be written with the pointer-sized `checked_write()` overload. Redirect destinations cannot point to read/write patch data. The framework does not provide generic executable allocation.

### `PatchAddress`

| Construction | Meaning |
|---|---|
| `PatchAddress{}` | Invalid address; validation fails if an operation uses it. |
| `PatchAddress::absolute(pointer)` | Absolute object or function pointer; null is rejected. |
| `PatchAddress::image_rva(rva)` | Address resolved against the selected image during preparation. |
| `AllocatedData::base()`, `element()`, `byte_offset()` | Address inside plan-owned allocated data. |

## Target access

The patch constructor receives:

```cpp
struct TargetContext {
    TargetLayout layout;
    HostRole role;
    ImageContext image;
};
```

`ImageContext` fields are `identity`, `architecture`, `base`, and `size`.

| Method | Result |
|---|---|
| `contains_rva(rva, extent = 1)` | Whether the complete range belongs to the image. |
| `address_at_rva(rva, extent = 1)` | Absolute address, or `0` when the range/address is invalid. |
| `function_at_rva<Function>(rva)` | Typed function pointer, or `nullptr`. |
| `read_at_rva<T>(rva)` | Aligned pointer to read-only game-owned state, or `nullptr`. |
| `read_at_rva<T>(rva, count)` | Aligned read-only span covering `count` objects, or an empty span. |
| `mutable_at_rva<T>(rva)` | Aligned pointer to mutable game-owned runtime state, or `nullptr`. |
| `mutable_at_rva<T>(rva, count)` | Aligned mutable span covering `count` runtime objects, or an empty span. |

Bounds checks establish only that an address is inside the recognized image. The plan must still validate every native helper, hook, write site, and ABI fact it relies on. `mutable_at_rva()` is for ordinary game-owned runtime state; installation writes belong to `PatchPlan`.

For verified unaligned fields in native objects and stack frames:

| Function | Result |
|---|---|
| `read_native_field<T>(object, offset = 0)` | Copy a trivially copyable field without alignment or aliasing assumptions. |
| `write_native_field(object, value)` | Copy a complete trivially copyable value into validated runtime storage. |
| `write_native_field(object, offset, value)` | Copy a trivially copyable value into a validated runtime field. |

These helpers do not validate pointers or native layouts. The patch must reject null owners and prove the relevant ABI before using them.

## Settings

The core owns INI generation and parsing. A patch declares typed metadata and receives a completed settings object; it does not read configuration files.

### Scalar settings

```cpp
setting("Key", &Settings::member, default_value)
```

Supported members are:

| `SettingKind` | C++ type |
|---|---|
| `Boolean` | `bool` |
| `SignedInteger` | Signed non-character integral type |
| `UnsignedInteger` | Unsigned non-character integral type |
| `FloatingPoint` | `float` or `double` |
| `String` | `std::string` |

The returned scalar builder supports:

| Builder | Applicable members | Effect |
|---|---|---|
| `.description(text)` | All | Optional generated comment. |
| `.range(minimum, maximum)` | Non-Boolean numeric | Inclusive range; the default must also fit. |
| `.max_length(bytes)` | `std::string` | Maximum UTF-8 byte length; the default must fit. |

Boolean input accepts `0/1`, `true/false`, and `off/on` case-insensitively. Numeric parsing is complete, locale-independent, and finite. Generated booleans use `0/1`.

Character integer types, arbitrary objects, maps, variants, nested dynamic values, and custom INI readers are not supported generic settings.

### Choice settings

Use a named enum when a setting has a finite set of modes:

```cpp
choice("Mode", &Settings::mode, Mode::Automatic,
       {ChoiceValue{"Automatic", Mode::Automatic},
        ChoiceValue{"Disabled", Mode::Disabled}})
    .description("How the patch selects its mode.")
```

`choice()` accepts its values as an initializer list, `std::array`, or `std::span` and returns a choice builder supporting `.description(text)`. The default must appear in the list. Names must be nonempty and unique under ASCII case-insensitive comparison; user input is matched the same way.

`choice_name(value, choices)` accepts the same initializer-list, `std::array`, or `std::span` metadata and returns the
matching user-facing name, or an empty `std::string_view` when the value is absent. It is useful for status output and
diagnostics that should use the same names as configuration.

### Settings groups

`settings_group()` places ordinary settings under `[PatchId.Group]`:

```cpp
settings_group<ControllerSettings>(
    "General",
    {setting("Rumble", &ControllerSettings::rumble, true)})
```

`keyed_string_group()` declares a finite set of known string keys, useful for mappings:

```cpp
keyed_string_group<ControllerSettings>(
    "Unit",
    &ControllerSettings::unit,
    {
        {.key = "A", .default_value = "Crouch", .description = "A button action.", .maximum_length = 32},
        {.key = "B", .default_value = "Roll", .maximum_length = 32},
    })
```

`KeyedStringSetting` fields are `key`, `default_value`, optional `description`, and `maximum_length`; zero means no explicit maximum. Users cannot add arbitrary keys through this API.

`keyed_string_group()` accepts its declarations as an initializer list, `std::array`, or `std::span`. Use an array for
large fixed maps so the metadata can remain separately named and readable.

`KeyedStrings` provides:

| Method | Result |
|---|---|
| `value(key) const noexcept` | Case-insensitive lookup returning `std::optional<std::string_view>`. |
| `values() const noexcept` | Completed fixed entries as `std::span<const KeyedStringValue>`. |

Each `KeyedStringValue` owns `key` and `value` strings. Group names must be nonempty and case-insensitively unique.

### Settings schema and validation

```cpp
SettingsSchema<MySettings>{
    .values = {...},
    .groups = {...},
    .validate = &validate_settings,
}
```

Fields appear in the order `values`, `groups`, `validate`. The optional validator has this type:

```cpp
std::expected<void, OutcomeReason> (*)(Settings&)
```

Use it once after generic conversion to validate relationships, normalize values, or derive typed state inside the settings object. It performs no file I/O, native-site validation, installation writes, or runtime mutation.

Expose the schema from the definition with:

```cpp
.settings = SettingsDefinition::from(SettingsSchema<MySettings>{...}),
```

Name `MySettings` as the fourth `make_patch_variant` template argument. `NoSettings` is the default marker when no dedicated values exist.

## Environment values

Environment values are patch-owned startup inputs. Read them only after Fusion Cutter has selected the patch; the
patch decides precedence and whether an invalid value disables behavior, warns, or fails preparation.

```cpp
auto count = read_environment_value<unsigned int>("SPAWN_TIMER");

constexpr std::array policies{
    ChoiceValue{"Disabled", DirectPolicy::Disabled},
    ChoiceValue{"PreferDirect", DirectPolicy::PreferDirect},
    ChoiceValue{"RequireDirect", DirectPolicy::RequireDirect},
};
auto policy = read_environment_choice("BF2_DIRECT_POLICY", policies);
```

Both helpers return `std::expected<std::optional<T>, OutcomeReason>`:

- an empty optional means the variable is absent;
- a value means parsing succeeded; and
- an unexpected result describes an unreadable, oversized, or malformed value.

Use `read_environment_value<std::string>()` for bounded raw text. Environment values accept at most 4096 bytes.
`read_environment_choice()` accepts its choices as an initializer list, `std::array`, or `std::span`. Scalar parsing
uses the same Boolean, integer, finite floating-point, and string rules as typed settings; choice matching is ASCII
case-insensitive. These helpers do not select patches, mutate settings automatically, log, or assign failure policy.

## Reporting

### Logging

`LogLevel` values are `Off`, `Error`, `Warning`, `Info`, and `Debug`.

All patch logging calls are `noexcept`:

```cpp
logging::write(level, source, message, operation = {}, related_patch = {});
logging::enabled(level);
logging::error(source, message, operation = {}, related_patch = {});
logging::warning(source, message, operation = {}, related_patch = {});
logging::info(source, message, operation = {}, related_patch = {});
logging::debug(source, message, operation = {}, related_patch = {});
```

Use `logging::enabled(level)` before assembling an optional or expensive diagnostic message. The write functions still
perform their own filtering.

`source` is the stable patch ID. `operation` and `related_patch` add optional structured context. Patches do not own log files, configure the backend, or include vendor logging types. The queue is bounded and dropping, but high-volume per-frame or per-packet logging is still inappropriate; prefer counters or rate-limited summaries.

### Status

Implement `StatusContributor::write_status(StatusSection&) const noexcept` for small live values. Add fields with:

```cpp
bool StatusSection::add(std::string_view label, std::string_view value) noexcept;
```

The return value indicates whether the full input fit. A section accepts at most 12 fields; labels have 48-byte capacity and values 192-byte capacity. Empty labels and excess fields are rejected. Oversized text is truncated safely, and carriage returns/newlines become spaces.

Status collection may read atomics or a small published snapshot. It performs no game/network calls, I/O, waiting, history accumulation, or expensive/unbounded work. Active contributors are polled at most once per second.

### ETL diagnostics

Permanent high-volume diagnostics publish compact records through the shared process trace:

```cpp
#include <FusionCutter/diagnostics.hpp>

diagnostics::EtlChannel channel;

channel.prepare(target, "ExampleDiagnostics", schema_version, capture_mode);
channel.start();
channel.submit(kind, payload, context, flags);
channel.omit();
channel.stop();
```

`prepare()` belongs in `prepare_runtime()`, `start()` in `enable_runtime()`, and `stop()` in `disable_runtime()`. All active channels in a process share one role-specific ETL file. Channel names and schema versions identify their records independently.

`submit()` accepts payloads through `diagnostics::kMaximumEtlPayloadSize` and copies them into a fixed per-thread producer ring. Larger payloads are rejected and counted as dropped. `context` is the channel-defined stable subject or transaction identifier; `kind` and `flags` belong to the channel schema. `omit(count)` reports evidence intentionally skipped by bounded aggregation or overflow handling.

Callbacks that submit records remain bounded and nonblocking: no allocation, formatting, file or ETW calls, locks, compression, waits, or high-cost clock queries. The core writer owns ETW and batching. `health()` returns `TraceHealth`; `filename()` returns the shared trace filename for status output. Diagnostics patches define typed payload helpers and record schemas privately rather than exposing ETW or vendor types to ordinary patches.

## Outcomes and failure reporting

### `OutcomeReason`

```cpp
struct OutcomeReason {
    std::string message;
    std::optional<std::string> operation;
    std::optional<PatchId> related_patch;
};
```

Use an actionable `message`. Set `operation` when one lifecycle stage or plan operation is relevant, and `related_patch` for dependency or conflict causality. Do not construct nested reason histories.

### Core initialization outcomes

| `InitializationOutcome` | Meaning |
|---|---|
| `Completed` | Core processing completed safely; individual patches may still have failed or skipped. |
| `Unsupported` | The target/role is not supported and no writes occurred. |
| `Fatal` | Core processing cannot safely continue. |

`InitializationResult` contains `outcome` and optional `reason`.

### Patch outcomes

| `PatchOutcome` | Meaning |
|---|---|
| `Disabled` | Applicable patch was not selected. |
| `NotApplicable` | No variant applies to the recognized environment. |
| `WaitingForImage` | A selected one-shot late image has not appeared. |
| `Installed` | Plan and runtime activation succeeded. |
| `Skipped` | A required dependency or earlier policy prevented installation. |
| `Failed` | Patch-local planning, validation, preparation, commit, or activation prerequisites failed. |

`PatchResult` contains `patch_id`, user-facing `name`, `outcome`, and optional `reason`.

## Build and verification tools

### Format

```powershell
./tools/format.ps1
./tools/format.ps1 -Check
```

The script requires clang-format major version 22 and covers project-owned C/C++ under `include`, `src`, `tests`, and `tools`.

### Configure and build

```powershell
cmake --preset vs2022-x86
cmake --build --preset vs2022-x86 --config RelWithDebInfo

cmake --preset vs2022-x64
cmake --build --preset vs2022-x64 --config RelWithDebInfo
```

The presets build universal client/server cores by default. Set `FC_CORE_ROLE=Client` or `FC_CORE_ROLE=Server` for an optional role-filtered artifact from the same source and ABI.

### Run tests

```powershell
ctest --test-dir build/vs2022-x86 -C RelWithDebInfo --output-on-failure
ctest --test-dir build/vs2022-x64 -C RelWithDebInfo --output-on-failure
```

Patch-owned tests mirror the patch path below `tests/patches/`. Use Catch2 through `Catch2::Catch2WithMain` and register the executable with CTest through `add_test`. The [RCON Server tests](../tests/patches/server/rcon_server/CMakeLists.txt) demonstrate the current pattern.

Simple checked writes and hooks normally rely on common plan tests and the verifier. Add patch-specific tests for consequential patch-owned behavior such as state machines, protocols, queues, complex conversions, or reproduced defects.

### Supported-binary verifier

```powershell
./build/vs2022-x86/artifacts/RelWithDebInfo/FusionCutter-Verify.exe <supported-image> [supported-image ...]
./build/vs2022-x64/artifacts/RelWithDebInfo/FusionCutter-Verify.exe <supported-image> [supported-image ...]
```

Use the executable matching the image architecture. The verifier privately maps each image, constructs every matching patch variant with compiled defaults, validates and reserves each plan, and mutates every declared critical dependency to prove rejection. It does not launch the game or modify the source file.

## Framework plumbing

Some public headers necessarily expose materialized catalog and type-erasure records used by the builders. They are
framework plumbing, not a second patch-author API. Use `make_patch_variant()`, `PatchVariants`,
`SettingsDefinition::from()`, and the concrete settings object received by the patch; do not manually construct or
retain factory, resolved-settings, catalog-envelope, or patch-instance records.

## Deliberately unavailable

These are intentional framework boundaries, not missing convenience APIs:

- direct installation-time game-memory writes;
- page-protection or instruction-cache management;
- direct SafetyHook types, ownership, or toggling;
- generic executable allocation;
- runtime signature scanning or another layout's RVA fallback;
- per-patch INI readers, arbitrary configuration maps, or runtime reload;
- runtime patch discovery or binary patch plugins;
- service locators, event buses, schedulers, generic managers, or callback-priority systems; and
- general hot enable, disable, unload, or restart.

When a capability appears absent, first check whether the plan, runtime lifecycle, settings, target, reporting, or ordinary C++ ownership already expresses it. A new shared capability requires project-level review; a patch must not bypass the core boundary locally.
