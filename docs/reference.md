# Patch Author Reference

This is the complete lookup for Fusion Cutter's patch-facing framework. Start with [Writing a Patch](patches.md) if you are new to the model; return here for exact names, signatures, and constraints.

## Quick lookup

| I want to... | Use | See |
|---|---|---|
| Register and compile a patch | `fc_patch()`, `fc_patch_sources()` | [Build manifest](#build-manifest) |
| Choose its startup or runtime model | `Patch`, `RuntimePatch`, `RuntimeOnlyPatch` | [Patch classes](#patch-classes) |
| Select releases, roles, images, and startup behavior | `make_patch_variant()`, `TargetContext` | [Variants](#variants) |
| Require or automatically select another patch | `depends_on`, `includes` | [`PatchDefinition`](#patchdefinition) |
| Validate, write, or remove native instructions | `BytePattern`, `checked_write()`, `nop()`, `require_bytes()` | [Writes and validation](#writes-and-validation) |
| Hook or redirect game code and call the original | `inline_hook()`, `mid_hook()`, `redirect_*()`, `OriginalFunction` | [Hooks](#hooks) |
| Allocate replacement data or access runtime state | `allocate_data()`, `ImageContext` | [Data allocation](#data-allocation) |
| Add ordinary, choice, grouped, or mapped settings | `setting()`, `choice()`, settings groups | [Settings](#settings) |
| Publish an instance or receive host updates | `PatchInstanceSlot<T>`, `Updatable` | [Callback bridge](#callback-bridge) |
| Add diagnostic events or live status | `logging::*`, `StatusContributor` | [Reporting](#reporting) |
| Return an actionable runtime failure | `OutcomeReason` | [Outcomes and failure reporting](#outcomes-and-failure-reporting) |
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
    std::span<const PatchId> depends_on;
    std::span<const PatchId> includes;
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
| `settings` | Optional typed settings schema; default construction means no dedicated settings. |
| `depends_on` | Patch IDs that must install successfully first. |
| `includes` | Companion patch IDs automatically selected with this patch; explicit disable wins. |
| `variants` | Supported layout, role, image, timing, implementation, and failure-policy combinations. |

Current shared categories from `src/patches/categories.hpp` are:

| Constant | Display name | Order |
|---|---|---|
| `categories::Limits` | Limits | `100` |
| `categories::Multiplayer` | Multiplayer | `200` |
| `categories::Server` | Server | `300` |

`PresentationCategory` has `name` and integer `order` fields.

`PatchId` is `std::string_view` and must refer to stable catalog-owned text.

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
make_patch_variant<PatchType, TargetLayout, Settings = NoSettings>(
    HostRole role,
    TargetImage image,
    ImageTiming image_timing = ImageTiming::Startup,
    StartupFailurePolicy failure_policy = StartupFailurePolicy::Local)
```

Collect them in `PatchVariants`:

```cpp
const PatchVariants kVariants{
    make_patch_variant<ExampleFix, TargetLayout::SteamRetail>(HostRole::Client, TargetImage::Game),
    make_patch_variant<ExampleFix, TargetLayout::GOGRetail>(HostRole::Client, TargetImage::Game),
};
```

Variants for the other architecture are removed at compile time. Do not reproduce this dispatch with compiler architecture macros in patch code.

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
| `BytePattern::exact(bytes)` | Require every bit to match. An empty mask represents exact matching. |
| `BytePattern{bytes, mask}` | Compare only mask bits: `(actual & mask) == (expected & mask)`. |
| `exact_pattern(value)` | Treat a trivially copyable value's object representation as an exact pattern. |

A pattern must contain at least one byte. A nonempty mask must have the same size as the byte span and constrain at least one bit. The plan copies pattern storage, so local arrays do not need to outlive `build_plan()`.

### Writes and validation

| Method | Use and requirements |
|---|---|
| `checked_write(operation, rva, BytePattern expected, span<byte> replacement)` | Replace bytes after validating the preimage. Expected and replacement sizes must match. |
| `checked_write(operation, rva, const T& expected, const T& replacement)` | Write one trivially copyable typed value using exact object bytes. |
| `checked_write(operation, rva, BytePattern expected, PatchAddress replacement)` | Write a resolved native pointer. The expected size must equal the target pointer size. |
| `nop(operation, rva, BytePattern expected)` | Replace the expected span with `0x90` bytes. |
| `require_bytes(operation, rva, BytePattern expected)` | Prove an unchanged native dependency and claim a read range. |

Read/read overlap is allowed. A mutation conflicting with another selected operation's mutation or required read causes a conflict. Do not duplicate validation with `require_bytes()` when an operation's own preimage already proves the fact.

### Hooks

| Method | Use and requirements |
|---|---|
| `inline_hook(operation, rva, expected, destination)` | Hook a whole function and return `OriginalFunction<Function>`. The preimage covers all instructions SafetyHook will replace. |
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
| `redirect_call(operation, rva, expected, destination)` | Convenience wrapper for `RedirectKind::Call`. |
| `redirect_call_with_original(operation, rva, expected, destination)` | Redirect an exact direct `E8` call and retain its decoded original target. |
| `redirect_jump(operation, rva, expected, destination)` | Convenience wrapper for `RedirectKind::Jump`. |

`RedirectKind` values are `Call` and `Jump`. Redirect preimages contain at least five replaceable bytes. The core writes `E8` or `E9`, calculates the displacement, and NOP-fills any remaining claimed bytes. It may create a near relay for an out-of-range x64 destination; an out-of-range x86 redirect fails.

`redirect_call_with_original()` requires the first five bytes to be fully constrained and to begin with `E8`.

### Original functions

`inline_hook()` and `redirect_call_with_original()` return `OriginalFunction<Function>`:

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
    std::optional<NearConstraint> proximity = std::nullopt)
```

`T` must be trivially copyable and non-const/non-volatile. The core allocates aligned read/write memory, zero-initializes it, copies any initial prefix, and owns it for the patch transaction/process lifetime.

`NearConstraint{rva, maximum_distance}` requests data near an image RVA. Its default maximum distance is `0x7FFF'FFFF`.

`AllocatedData<T>` is the returned symbolic handle:

| Method | Result |
|---|---|
| `size()` | Element count. |
| `explicit operator bool()` | Whether the handle owns a symbolic slot, not whether preparation has run. |
| `base()` | Symbolic base `PatchAddress`. |
| `element(index)` | Symbolic address of an in-range element; otherwise invalid. |
| `offset(byte_offset)` | Symbolic byte-offset address; plan validation rejects an out-of-range use. |
| `data()` | Runtime `T*` after successful preparation, or `nullptr` before allocation/release. Never dereference it in `build_plan()`. |

Allocation addresses may be written with the pointer-sized `checked_write()` overload. Redirect destinations cannot point to read/write patch data. The framework does not provide generic executable allocation.

### `PatchAddress`

| Construction | Meaning |
|---|---|
| `PatchAddress{}` | Invalid address; validation fails if an operation uses it. |
| `PatchAddress::absolute(pointer)` | Absolute object or function pointer; null is rejected. |
| `PatchAddress::image_rva(rva)` | Address resolved against the selected image during preparation. |
| `AllocatedData::base()`, `element()`, `offset()` | Address inside plan-owned allocated data. |

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
| `contains_rva(rva, extent)` | Whether the complete range belongs to the image. |
| `address_at_rva(rva, extent = 1)` | Absolute address, or `0` when the range/address is invalid. |
| `function_at_rva<Function>(rva)` | Typed function pointer, or `nullptr`. |
| `read_at_rva<T>(rva)` | Aligned pointer to read-only game-owned state, or `nullptr`. |
| `mutable_at_rva<T>(rva)` | Aligned pointer to mutable game-owned runtime state, or `nullptr`. |

Bounds checks establish only that an address is inside the recognized image. The plan must still validate every native helper, hook, write site, and ABI fact it relies on. `mutable_at_rva()` is for ordinary game-owned runtime state; installation writes belong to `PatchPlan`.

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

`choice()` returns a choice builder supporting `.description(text)`. The default must appear in the list. Names must be nonempty and unique under ASCII case-insensitive comparison; user input is matched the same way.

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

Name `MySettings` as the third `make_patch_variant` template argument. `NoSettings` is the default marker when no dedicated values exist.

## Reporting

### Logging

`LogLevel` values are `Off`, `Error`, `Warning`, `Info`, and `Debug`. `compiled_default_log_level()` is `Debug` in Debug builds and `Error` otherwise.

All patch logging calls are `noexcept`:

```cpp
logging::write(level, source, message, operation = {}, related_patch = {});
logging::error(source, message, operation = {}, related_patch = {});
logging::warning(source, message, operation = {}, related_patch = {});
logging::info(source, message, operation = {}, related_patch = {});
logging::debug(source, message, operation = {}, related_patch = {});
```

`source` is the stable patch ID. `operation` and `related_patch` add optional structured context. Patches do not own log files, configure the backend, or include vendor logging types. The queue is bounded and dropping, but high-volume per-frame or per-packet logging is still inappropriate; prefer counters or rate-limited summaries.

### Status

Implement `StatusContributor::write_status(StatusSection&) const noexcept` for small live values. Add fields with:

```cpp
bool StatusSection::set(std::string_view label, std::string_view value) noexcept;
```

The return value indicates whether the full input fit. A section accepts at most 12 fields; labels have 48-byte capacity and values 192-byte capacity. Empty labels and excess fields are rejected. Oversized text is truncated safely, and carriage returns/newlines become spaces.

Status collection may read atomics or a small published snapshot. It performs no game/network calls, I/O, waiting, history accumulation, or expensive/unbounded work. Active contributors are polled at most once per second.

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

## Framework-owned public types

These types are public because the catalog and configuration adapters use them. Ordinary patch implementations normally encounter them only through the builders documented above.

### Variant and factory plumbing

| Type or function | Purpose |
|---|---|
| `PatchVariant` | Materialized layout, role, image, timing, failure policy, and factory. |
| `PatchVariants<...>` | Architecture-filtered descriptor collection convertible to `span<const PatchVariant>`. |
| `PatchFactory` | Settings type plus catalog construction function. |
| `patch_factory<PatchType, Settings>()` | Creates the type-erased factory and verifies constructor shape. |
| `PatchInstance` | `variant<unique_ptr<Patch>, unique_ptr<RuntimeOnlyPatch>>`. |
| `PatchBuildEnvelope` | Generated artifact booleans `x86`, `x64`, `client`, `server` with `supports(Architecture)` and `supports(HostRole)`. |

Patch authors should use `make_patch_variant()` and `PatchVariants` instead of constructing these records manually.

### Settings metadata and type erasure

| Type | Public surface |
|---|---|
| `SettingMetadata` | `group`, `key`, `description`, `kind`, `default_value`, `choices`. |
| `SettingEntry<Settings>` | Metadata, default applier, value applier, optional metadata error. |
| `SettingsGroup<Settings>` | Group `name` and typed `values`. |
| `SettingsValidator<Settings>` | Validator function-pointer alias. |
| `ChoiceValue<Choice>` | User-facing `name` and enum `value`. |
| `KeyedStringSetting` | `key`, `default_value`, `description`, `maximum_length`. |
| `KeyedStringValue` | Completed owned `key` and `value`. |

`SettingsDefinition` normally appears only as `SettingsDefinition::from(schema)`. Its complete public surface is:

| Method | Core use |
|---|---|
| default constructor | Represents `NoSettings`. |
| `from(SettingsSchema<Settings>)` | Type-erases one schema. |
| `settings_type()` | Returns the schema `type_index`, or `NoSettings`. |
| `metadata()` | Returns flattened metadata. |
| `validate_metadata()` | Checks builder and uniqueness constraints. |
| `make_defaults()` | Creates resolved default settings. |
| `find(group, key)` | Case-insensitive lookup returning an optional index. |
| `apply(settings, index, value)` | Converts and applies one configuration value. |
| `validate(settings)` | Runs the patch validator. |

`ResolvedSettings` is movable, noncopyable type-erased factory plumbing. Its public operations are `make(value)`, `is<Settings>()`, and rvalue-only `take<Settings>()`. Patch constructors receive the concrete type and should not use `ResolvedSettings` directly.

`PatchPlan(PatchId, ImageContext)` is movable and noncopyable, but patch code does not construct or retain plans.

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
