# Writing a Patch

Fusion Cutter turns an existing game modification into a source patch that is applied when the game starts. If your mod is currently an executable hex edit, the basic idea is familiar: identify the original bytes, describe the replacement, and tell Fusion Cutter which game versions can use it.

The framework handles the work around that edit. It recognizes the exact game binary, reads the user's configuration, validates the original bytes, detects conflicts with other patches, changes memory safely, and reports the result. Your patch owns the game-specific knowledge: what behavior changes, where it lives, and what proves that the change is safe for each supported version.

This guide introduces the framework pieces a patch author normally uses. The [Patch Author Reference](reference.md) is the exhaustive API lookup.

## From a hex edit to a patch

Suppose your mod changes this instruction:

```text
Original:    74 05
Replacement: EB 05
```

A Fusion Cutter patch describes the same operation in C++:

```cpp
void build_plan(PatchPlan& plan) override {
    constexpr auto kOriginal = byte_array<0x74, 0x05>();
    constexpr auto kReplacement = byte_array<0xEB, 0x05>();

    plan.checked_write("Accept corrected state", 0x00123456, BytePattern::exact(kOriginal), kReplacement);
}
```

This does not immediately write to the game. It adds a checked operation to a plan. The core later verifies the address and original bytes alongside every other selected patch before committing anything. A wrong binary, stale address, or conflicting patch therefore produces a controlled failure instead of an unverified write.

### Addresses are RVAs

The address passed to a plan operation is a relative virtual address, or RVA, within the selected game image. It is not a raw file offset and it is not the process's final runtime address.

For example, if a function appears at `0x00523456` in a reviewed executable whose preferred image base is `0x00400000`, its RVA is `0x00123456`. PE file offsets and RVAs describe different layouts and are not generally interchangeable. Fusion Cutter combines the RVA with the actual loaded image base at runtime.

Keep each RVA with the target layout it was reviewed against. The corresponding code may move between Steam, GOG, Mod Tools, and the Classic Collection even when the surrounding function looks similar.

Most patches begin with two files in a folder below [`src/patches`](../src/patches/):

```text
patch.cmake
patch.cpp
```

Small patches may remain entirely in those two files. [Input Update Rate](../src/patches/limits/input_update_rate/patch.cpp) is a complete example.

## `patch.cmake`: introduce the patch to the build

`patch.cmake` is the patch's build manifest. Fusion Cutter finds these manifests automatically and uses them to generate its patch catalog.

```cmake
fc_patch(
    ID ExampleFix
    DEFINITION fusioncutter::patches::example_fix::definition
    ARCHITECTURES X86
    ROLES CLIENT
    SOURCES
        patch.cpp
)
```

The manifest answers a few build-time questions:

- `ID` is the permanent identity used by configuration, logs, status, and patch relationships.
- `DEFINITION` points to the C++ definition in `patch.cpp`.
- `ARCHITECTURES` and `ROLES` decide which Fusion Cutter builds contain the patch.
- `SOURCES` lists the files needed to compile it.

This is only the artifact envelope. The C++ variants described below decide whether the patch applies to Steam, GOG, Mod Tools, or the Classic Collection at runtime.

## `patch.cpp`: describe the patch

For a small patch, `patch.cpp` contains three ideas:

1. A **patch class** describes the work to perform.
2. **Variants** describe the game versions and roles supported by that class.
3. A **patch definition** describes the feature to users and to the framework.

Here is a complete but fictional one-file patch:

```cpp
#include <FusionCutter/categories.hpp>
#include <FusionCutter/patch.hpp>

namespace fusioncutter::patches::example_fix {
namespace {

class ExampleFix final : public Patch {
  public:
    explicit ExampleFix(const TargetContext&) noexcept {}

    void build_plan(PatchPlan& plan) override {
        constexpr auto kOriginal = byte_array<0x74, 0x05>();
        constexpr auto kReplacement = byte_array<0xEB, 0x05>();

        plan.checked_write("Accept corrected state", 0x00123456, BytePattern::exact(kOriginal), kReplacement);
    }
};

const PatchVariants kVariants{
    make_patch_variant<ExampleFix, TargetLayout::SteamRetail>(HostRole::Client, TargetImage::Game),
};

} // namespace

PatchDefinition definition() {
    return {
        .name = "Example Fix",
        .enabled = true,
        .configurable = true,
        .category = categories::Multiplayer,
        .description = "Correct the example game behavior.",
        .variants = kVariants,
    };
}

} // namespace fusioncutter::patches::example_fix
```

The class is not a loader or an independently discovered plugin. The generated catalog constructs it only after the core recognizes a matching target and resolves its settings. `build_plan()` then describes everything the patch wants to validate or change in the selected game image.

The definition supplies the user-facing name, default state, configuration visibility, category, optional description, settings, relationships, and variants. The manifest `ID` remains the stable machine-facing name; `.name` is the readable name shown to players and server operators.

## Which patch class should I use?

The right class depends on what must remain alive after installation.

### `Patch`: a startup change

Use `Patch` for a patch that can be completely expressed as checked memory operations. This is the closest equivalent to an executable hex edit. Implement `build_plan()` and let the core own validation, installation, and rollback.

Examples include changing a constant, replacing instructions, redirecting a call, or allocating a larger table and updating the game's pointer to it. [Input Update Rate](../src/patches/limits/input_update_rate/patch.cpp) and [DLC Mission Limit](../src/patches/limits/dlc_mission_limit/) demonstrate this form.

### `RuntimePatch`: a change with living code or state

Use `RuntimePatch` when the memory plan routes the game into your C++ code or when the patch keeps a socket, worker, queue, or other runtime state.

It still has `build_plan()`, but may also participate in three lifecycle steps:

- `prepare_runtime()` creates fallible resources while the patch is still inactive.
- `enable_runtime()` makes already prepared callbacks or services available after the memory plan commits.
- `disable_runtime()` closes that access before rollback or shutdown.

This ordering prevents the game from reaching half-prepared code. [Colored Chats](../src/patches/multiplayer/colored_chats/) is a compact callback example; [RCON Server](../src/patches/server/rcon_server/) is a larger runtime service.

When preparation cannot complete, return an actionable reason and leave the patch inactive:

```cpp
std::expected<void, OutcomeReason> prepare_runtime() override {
    if (!start_service()) {
        return std::unexpected(OutcomeReason{
            .message = "Could not start the network service.",
            .operation = "Start service",
        });
    }
    return {};
}
```

The core records that failure in the patch result, status output, and enabled logging. The patch does not need to log the same failure separately.

### `RuntimeOnlyPatch`: a service without a memory edit

Use `RuntimeOnlyPatch` for runtime behavior that does not need to modify a game image. It has the same prepare, enable, and disable model as `RuntimePatch`, but no `build_plan()`. This exists for services that can be started entirely through an existing framework or host entry point; most native patches will use one of the first two classes.

Runtime patches can additionally implement `Updatable` when they need a bounded, nonblocking host update callback, or `StatusContributor` when they have useful live values for the status file. These are optional capabilities, not requirements for every runtime patch.

## Supporting more than one game version

Steam, GOG, Mod Tools, and the Classic Collection often contain the same feature at different addresses. They may also implement part of the feature differently. Fusion Cutter represents these as variants of one logical patch, so users still see one `ExampleFix` rather than a separate patch for every release.

```cpp
const PatchVariants kVariants{
    make_patch_variant<ExampleFix, TargetLayout::SteamRetail>(HostRole::Client, TargetImage::Game),
    make_patch_variant<ExampleFix, TargetLayout::GOGRetail>(HostRole::Client, TargetImage::Game),
    make_patch_variant<ExampleFix, TargetLayout::ModTools>(HostRole::Client, TargetImage::Game),
    make_patch_variant<ClassicExampleFix, TargetLayout::Aspyr>(HostRole::Client, TargetImage::Game),
};
```

Several variants may construct the same class when only the layout data changes. A genuinely different implementation can use another class while remaining part of the same patch definition. The class constructor receives `TargetContext`, whose layout, role, and image identify the selected environment.

Every native address and assumption must belong to the layout it was reviewed against. Fusion Cutter intentionally does not search for a similar function in unknown binaries or borrow another version's addresses.

## Choosing the role, image, and startup behavior

A variant says more than which release it supports. It also tells the core where and when the patch belongs.

`HostRole::Client` and `HostRole::Server` distinguish the playable game from a dedicated server. A patch may support either or both roles, and it may use a different class for each when their native implementations differ.

`TargetImage` identifies the physical binary being changed:

- `Game` is the main game image used by most patches.
- `Bootstrap` is the Classic Collection bootstrap executable.
- `GalaxyPeer` is the optional Galaxy peer library.

The image is normally present during startup. If an optional image loads later, `ImageTiming::OneShotLate` tells the core to install its patch once when that image appears. Work across two physical images is represented by related patches because each patch plan owns one image.

Most failures remain local: the affected patch and anything depending on it do not install, while unrelated patches continue. `StartupFailurePolicy::StartupRequired` is available for a server feature whose applicable variant must work for the server to be considered safely initialized. It is not appropriate for an optional client patch or a late image that may never load.

## What can my patch ask the core to do?

`PatchPlan` covers the fundamental operations used by the current patches:

- **Change bytes or typed values** with `checked_write()`.
- **Remove instructions** with `nop()`.
- **Verify an unchanged native dependency** with `require_bytes()`.
- **Redirect a call or jump** with `redirect_call()` or `redirect_jump()`.
- **Hook a whole function** with `inline_hook()`, or use `inline_hook_with_original()` when the callback must call the
  original function.
- **Hook an instruction inside a function** with `mid_hook()` and inspect or adjust the saved CPU context.
- **Allocate read/write data** with `allocate_data()` and refer to it symbolically from other operations.

Each operation includes the expected native bytes. The core uses those expectations as the proof that the address still means what the patch thinks it means.

For example, a typed constant needs no manual byte conversion:

```cpp
constexpr std::uint8_t kOriginalRate = 30;
constexpr std::uint8_t kNewRate = 120;
plan.checked_write("Raise input update rate", rate_rva_, kOriginalRate, kNewRate);
```

Exact patterns are the normal choice. A masked pattern is useful when some bits are expected to vary while the surrounding instruction still proves the site's identity. `require_bytes()` can prove a separate instruction or native dependency that the operation itself does not cover. Avoid listing every value another executable patch might have written; validate the stable native facts that make your operation safe.

Hooks and redirects also depend on the game's native ABI. A whole-function hook or original function uses a function-pointer type whose return value, parameters, and calling convention match the reviewed game function:

```cpp
using NativeFunction = int(__cdecl*)(std::uint32_t handle) noexcept;
OriginalFunction<NativeFunction> original_;
```

Use a mid-function hook when the patch belongs at a reviewed instruction boundary rather than at a callable function entry. Hook callbacks are `noexcept`, bounded, and prepared before activation; they do not perform blocking work or recurring address discovery.

The [plan API reference](reference.md#memory-and-hook-tools) lists every operation and its safety rules.

## How do I add settings for my patch?

Every configurable patch can be enabled or disabled without defining any dedicated settings. Add settings when a player should also choose a value. The patch describes those values once; the core generates the appropriate INI entries, parses user input, applies defaults, and gives the completed settings object to the patch constructor.

For a configurable frame-rate limit, the patch can define:

```cpp
struct UnlockFrameRateSettings {
    std::uint32_t max_frame_rate{};
};
```

The definition explains how that member appears in the configuration:

```cpp
.settings = SettingsDefinition::from(SettingsSchema<UnlockFrameRateSettings>{
    .values = {
        setting("MaxFrameRate", &UnlockFrameRateSettings::max_frame_rate, std::uint32_t{120})
            .description("Maximum rendered frames per second; 0 removes the limit.")
            .range(0, std::numeric_limits<std::uint32_t>::max()),
    },
}),
```

Name the settings type in each applicable variant, then receive it as the first constructor argument:

```cpp
make_patch_variant<UnlockFrameRate, TargetLayout::SteamRetail, UnlockFrameRateSettings>(
    HostRole::Client, TargetImage::Game)

UnlockFrameRate(UnlockFrameRateSettings settings, const TargetContext& target) noexcept;
```

Settings may be booleans, integers, floating-point values, strings, or named enum choices. Related values can be placed in named groups, and a finite set of known string keys can represent mappings such as controller bindings. A patch may validate relationships between its completed values, but it never opens or parses the INI itself.

Most variants share the definition's schema. If one role or target needs different settings, pass its schema directly
to that `make_patch_variant()` call; only the matching configuration receives those entries. A launcher or server tool
may also provide a startup override through the shared environment helpers. The patch reads that value after selection
and owns its precedence and failure behavior; the framework toggle remains the outer gate.

See [Settings](reference.md#settings) for every builder and grouping option.

## What if my patch relies on another patch?

Patch relationships live in the definition:

```cpp
.depends_on = {"RequiredPatch"},
.includes = {"HelpfulCompanion"},
```

`depends_on` means this patch cannot install unless the named patch installs first. `includes` automatically selects a companion patch, but does not make the including patch fail when that companion cannot install. An explicit user disable still wins.

A dependency-only feature is still an ordinary patch. It can be nonconfigurable so it does not clutter the user's INI.

Relationships are also how one user-facing feature can coordinate work in more than one physical image. The main patch can include a nonconfigurable companion for the second image without inventing a special patch type or giving that companion an unnecessary user setting.

## How do callbacks reach my patch object?

Native hooks usually require a free or static function, while useful state belongs to the patch instance. `PatchInstanceSlot<T>` is the narrow bridge between them. The runtime patch publishes itself during `enable_runtime()`, the callback reads the active pointer, and `disable_runtime()` clears it.

```cpp
void enable_runtime() noexcept override {
    active_.publish(*this);
}

void disable_runtime() noexcept override {
    active_.clear(*this);
}

static void callback() noexcept {
    if (auto* patch = active_.read()) {
        patch->handle_callback();
    }
}
```

The `nullptr` path is important: callbacks must remain safe while the patch is inactive or shutting down. An inline hook or redirected call can also return `OriginalFunction<Fn>`, which becomes callable after successful installation.

## How does my patch report useful information?

Installation results are already written to Fusion Cutter's shared status and log outputs. A patch does not need to create its own files or repeat ordinary success and failure reporting.

Use the `logging` functions for additional events that help diagnose the patch. The stable patch ID is the log source, and an operation or related patch can be attached as structured context. Logging is bounded but is not intended for every frame or packet.

Implement `StatusContributor` when the patch owns a small live value that remains useful after startup, such as a service connection state or active peer. `write_status()` fills a bounded `StatusSection` from atomics or a prepared snapshot. It must not query the game, perform network or file I/O, or wait while status is being collected.

For frequent runtime activity, keep counters or a compact published snapshot and report summaries. The [reporting reference](reference.md#reporting) contains the available logging calls and status limits.

## Checking the patch

Build the architecture that contains the patch, run the focused tests, and use the supported-binary verifier against every binary the new variants claim to support:

```powershell
cmake --preset vs2022-x86
cmake --build --preset vs2022-x86 --config RelWithDebInfo
ctest --test-dir build/vs2022-x86 -C RelWithDebInfo --output-on-failure

./build/vs2022-x86/artifacts/RelWithDebInfo/FusionCutter-Verify.exe <supported-image>
```

The verifier constructs matching variants from their default settings, validates and reserves their plans, and proves that changed critical bytes are rejected. It does not launch the game or modify the supplied executable.

A straightforward checked write or hook normally needs no patch-specific unit test beyond this common verification. Add a focused Catch2 test when the patch owns consequential logic such as a state machine, protocol parser, bounded queue, complex conversion, or a reproduced defect.

Follow [CONTRIBUTING.md](../CONTRIBUTING.md) for project rules and required validation.

## Examples worth reading

| Patch | What it demonstrates |
|---|---|
| [Input Update Rate](../src/patches/limits/input_update_rate/patch.cpp) | A complete one-file patch and target-specific RVA selection. |
| [Unlock Frame Rate](../src/patches/limits/unlock_frame_rate/) | A numeric user setting shared across retail and Mod Tools variants. |
| [DLC Mission Limit](../src/patches/limits/dlc_mission_limit/) | Core-owned data allocation and pointer replacement. |
| [Crouch Bug Fix](../src/patches/multiplayer/crouch_bug_fix/) | A mid-function hook using saved CPU context. |
| [Colored Chats](../src/patches/multiplayer/colored_chats/) | Settings, an inline hook, an original function, and a published runtime instance. |
| [RCON Server](../src/patches/server/rcon_server/) | A larger multi-target runtime service with role- and architecture-specific sources. |

Once the model is familiar, keep the [Patch Author Reference](reference.md) open as the complete lookup for signatures, constraints, and less common capabilities.
