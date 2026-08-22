# Writing Your First FusionCutter Patch

FusionCutter turns a game modification into a patch that can be enabled, configured, checked against the exact game
binary, and installed safely at startup.

If your mod begins as a hex edit, you already know the most important facts:

- which game executable you studied;
- where the change belongs;
- what the original bytes or value are; and
- what should replace them.

Your patch supplies that game-specific knowledge. FusionCutter owns executable recognition, configuration, validation,
conflict detection, native installation, and shared reporting.

This guide explains how to express those facts with the FusionCutter SDK. It assumes you can edit and build a small C++
project, but it does not assume prior FusionCutter knowledge. In the examples, `fc::` means the type or function comes
from FusionCutter.

This guide uses *native code* and *native data* to mean the compiled instructions and values in the loaded game image.

Use [SDK_REFERENCE.md](SDK_REFERENCE.md) when you need an exact signature. Read
[BEST_PRACTICES.md](BEST_PRACTICES.md) before releasing a patch.

## What do you want your patch to do?

Start with the result you want:

| Goal | Start with |
| --- | --- |
| Replace instructions or make a hex edit | `CodeLocation` and `Plan::write` |
| Change a number or pointer | `DataLocation` and `Plan::write` |
| Remove instructions | `Plan::nop` |
| Send an existing call somewhere else | `CallLocation` and `Plan::redirect_call` |
| Run your C++ code when the game calls a function | `FunctionLocation` and `Plan::hook` |
| Keep counters, services, or other state while the game runs | A handler created with `fc::patch` |
| Let the user choose a value | A typed settings schema |

The first patch below is a checked hex edit. The later sections build on it for other kinds of changes.

## How a FusionCutter patch fits together

Before writing code, it helps to know what each framework term represents:

| Part | What it means |
| --- | --- |
| Plugin | One DLL that contributes one or more patches to FusionCutter. |
| Patch | One feature with its own ID, enable setting, supported targets, behavior, and result. |
| Support | The game layout, client or server role, and executable image where one patch implementation applies. |
| Settings | User configuration that FusionCutter checks and converts into a C++ object for the patch. |
| Handler | A C++ object that keeps a patch's callbacks, resources, or other state alive after startup. |
| Plan | The patch's requested native checks and changes, collected before the game is modified. |
| Location | A description of code, data, a function, a call, or a vtable at an RVA in the selected image. |
| Evidence | The original bytes, value, pointer, or branch target expected at a location. |

A simple hex edit uses a patch, a support, and a Plan. It does not need an author-defined handler because no private
state or callback must remain alive.

### What happens when the patch starts

FusionCutter processes an enabled patch in stages:

1. **Selection and settings:** it selects a matching support and reads the patch's configuration.
2. **Create:** it creates the patch handler and gives it the completed settings. `fc::plan_patch` supplies this simple
   plumbing when the author does not need a handler class.
3. **Plan:** it asks the patch to describe its requirements, writes, redirects, hooks, and native allocations. Nothing
   in the game is changed during this stage.
4. **Validation:** it checks locations, evidence, dependencies, and conflicts across all selected Plans.
5. **Prepare:** it creates planned native storage and calls the handler's optional `prepare` method for resources or
   lookups that may fail. Native changes are still not visible to the game.
6. **Commit:** the framework installs the validated writes and hooks as a native transaction. Patch authors do not
   implement a Commit method.
7. **Activate:** after Commit succeeds, the handler's optional `activate` method can expose already prepared runtime
   behavior.

After startup, an optional `update` method can perform small housekeeping tasks, and `write_status` can publish current
state. Most patches implement only the stages they need; a checked startup edit usually supplies only a Plan.

## Gather the facts for your edit

Suppose your mod changes this instruction:

```text
Original:    74 05
Replacement: EB 05
```

You also need the address relative to the start of the executable image. FusionCutter calls this an RVA, or relative
virtual address. It is not a file offset and not the final address seen while the game runs.

For example:

```text
Address shown by the disassembler: 0x00523456
Preferred image base:               0x00400000
RVA used by the patch:              0x00123456
```

Keep the RVA and original bytes with the exact executable revision you inspected. Code can move between Steam, GOG,
Mod Tools, and the Classic Collection.

## Create a plugin project

The first example builds one plugin DLL containing one patch.

You need Visual Studio 2022 with the MSVC v143 toolset, CMake 3.28 or newer, and an installed FusionCutter SDK. Use the
x86 SDK for x86 plugins and the x64 SDK for x64 plugins.

Start with two files:

```text
ExamplePatches/
|-- CMakeLists.txt
`-- src/
    `-- plugin.cpp
```

Create `CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.28)

project(ExamplePatches LANGUAGES CXX)

find_package(FusionCutterSDK CONFIG REQUIRED)

fc_add_plugin(ExamplePatches
    FACTORY example::build_plugin
    SOURCES
        src/plugin.cpp
)
```

`find_package` loads the SDK. `fc_add_plugin` builds the DLL, applies the supported compiler settings, links the SDK,
and exports `example::build_plugin` as the plugin's entry point.

If you built FusionCutter yourself, install its SDK first. For an x86 build:

```powershell
cmake --install C:\path\to\FusionCutter\build\vs2022-x86 `
    --config RelWithDebInfo `
    --prefix C:\path\to\FusionCutterSDK-x86
```

## Write a checked hex edit

Create `src/plugin.cpp` by adding the following pieces in order.

Start with the SDK header and the native facts from the executable you reviewed:

```cpp
#include <FusionCutter/SDK.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

namespace example {

constexpr fc::Rva kExampleFixRva{0x00123456};

constexpr std::array kOriginalInstruction{
    std::byte{0x74},
    std::byte{0x05},
};

constexpr std::array kReplacementInstruction{
    std::byte{0xeb},
    std::byte{0x05},
};
```

`Rva` identifies a position relative to the loaded image. The two arrays preserve the expected instruction and its
replacement as separate facts. Their values are fictional here; use values from the exact executable you reviewed.

Next, describe where this implementation is allowed to run:

```cpp
[[nodiscard]] fc::Support steam_client_game() {
    return fc::support({
        .layouts = {fc::TargetLayout::SteamRetail},
        .roles = fc::HostRole::Client,
        .image = fc::TargetImage::Game,
    });
}
```

A support is a selection rule. This one allows the patch only for the Steam retail layout, the playable client, and
the main game executable. FusionCutter also recognizes the exact executable revision before this support can be used;
the support does not make the same RVA valid for every Steam executable.

Now describe the user-visible patch and the native work it needs:

```cpp
[[nodiscard]] fc::Patch example_fix() {
    return fc::plan_patch(
        {
            .id = "ExampleFix",
            .name = "Example fix",
            .enabled = true,
            .description = "Corrects an example branch in the Steam client.",
            .supports = {steam_client_game()},
        },
        [](fc::Plan& plan) {
            plan.write(
                fc::CodeLocation{
                    .rva = kExampleFixRva,
                    .name = "AcceptCorrectedState",
                    .evidence = fc::exact_bytes(kOriginalInstruction),
                },
                kReplacementInstruction);
        });
}
```

`fc::plan_patch` is the concise form for a patch that does not need its own long-lived handler. The patch definition
provides the permanent ID, display text, default enable state, and supports. FusionCutter uses the ID in configuration,
logging, status, and patch relationships.

The lambda is the patch's Plan stage. `plan.write` records a requested code change; it does not write memory while the
lambda runs. `CodeLocation` tells FusionCutter that the RVA contains executable code, while `exact_bytes` supplies the
evidence that must match. The framework can therefore reject a stale address, wrong executable, or overlapping patch
before Commit.

Finally, return the plugin that contains the patch and close the namespace:

```cpp
fc::Plugin build_plugin() {
    return fc::plugin({
        .id = "ExamplePatches",
        .version = "0.1.0",
        .author = "Your name",
        .patches = {example_fix()},
    });
}

} // namespace example
```

`build_plugin` is the function named by `FACTORY` in `CMakeLists.txt`. Its plugin definition gives the DLL contribution
a permanent ID and lists every patch it contributes. Add more patch factory functions to `.patches` as the plugin
grows.

## Build and try the plugin

Configure and build the x86 plugin:

```powershell
cmake -S . -B build/x86 `
    -G "Visual Studio 17 2022" `
    -A Win32 `
    -T v143 `
    "-DCMAKE_PREFIX_PATH=C:\path\to\FusionCutterSDK-x86"

cmake --build build/x86 --config RelWithDebInfo
```

Use `-A x64`, a separate build directory, and the x64 SDK for an x64 plugin.

Copy the DLL to `plugins/ExamplePatches.dll` under the directory containing `FusionCutter.dll`. FusionCutter generates
`config/FC.ExamplePatches.ini`:

```ini
[General]
ExampleFix=true
```

Every configurable patch receives an enable switch. Extra values are added with a settings schema later in this guide.

At this point, the example is a complete checked patch. The remaining sections show how the same framework model
handles other goals. Continue with the parts that match your own mod.

## Change a number or other native value

The remaining examples are focused fragments. Put location declarations beside the constants in `plugin.cpp`. Put
calls beginning with `plan.` inside a Plan callback such as the lambda in `example_fix()`.

Use `DataLocation<T>` when an RVA contains a C++ value of type `T`. This example changes a 32-bit player limit:

```cpp
const fc::DataLocation<std::uint32_t> maximum_players{
    .rva = fc::Rva{0x00134560},
    .name = "MaximumPlayers",
    .evidence = fc::expect(std::uint32_t{32}),
};

plan.write(maximum_players, std::uint32_t{64});
```

The type supplies the size and alignment. `fc::expect` records the original typed value. During Validation,
FusionCutter resolves the RVA and checks that it still contains `32`; during Commit, it replaces that value with `64`.

For a fixed array, add the element count and derive the element you want:

```cpp
const fc::DataLocation<std::uint32_t, 4> team_scores{
    .rva = fc::Rva{0x00135000},
    .name = "TeamScores",
};

const auto second_score = fc::element(
    team_scores,
    1,
    {.name = "SecondTeamScore", .evidence = fc::expect(std::uint32_t{0})});
```

`element` derives another location; it does not read the array. The derived location keeps the parent image and type,
computes the correct byte offset, and carries the evidence for that element.

## Replace or remove instructions

The first patch used `plan.write` to replace code. Use `plan.nop` when the goal is to remove complete instructions:

```cpp
const fc::CodeLocation optional_check{
    .rva = fc::Rva{0x00105060},
    .name = "OptionalStateCheck",
    .evidence = fc::exact_bytes({std::byte{0x75}, std::byte{0x08}}),
};

plan.nop(optional_check, 2);
```

The byte count must cover complete reviewed instructions. The Plan claims and validates that complete code range, then
Commit replaces it with NOP instructions. FusionCutter is not an assembler; use explicit replacement bytes when you
need a different instruction sequence.

## Send an existing function call somewhere else

First describe the game function's C++ signature. Then describe both the original function and the call instruction:

```cpp
struct GameState;
using Update = void (*)(GameState*) noexcept;

void replacement_update(GameState*) noexcept {
    // Implement the replacement behavior here.
}

const fc::FunctionLocation<Update> original_update{
    .rva = fc::Rva{0x00201000},
    .name = "UpdateGameState",
};

const fc::CallLocation<Update> update_call{
    .rva = fc::Rva{0x00104050},
    .name = "UpdateGameStateCall",
    .evidence = fc::direct_call_to(original_update),
};

const Update original_target = plan.redirect_call(update_call, &replacement_update);
```

The `CallLocation` RVA is the address of the call instruction. `FunctionLocation` describes the function it originally
calls, and `direct_call_to` makes that original target part of the evidence. FusionCutter decodes and revalidates the
existing call, reserves its bytes against conflicts, and changes its target during Commit.

`redirect_call` returns the original target so runtime code can preserve the old behavior when needed. The pointer can
be retained for later use, but it must not be called while the Plan is being built.

The signature must match the game's calling convention, arguments, and return value. See
[Native calls](SDK_REFERENCE.md#native-calls) when an ordinary function pointer cannot describe the game function.

## Replace a vtable slot

Using the function types from the previous example, start with the vtable RVA, derive the slot, and write a matching
function pointer:

```cpp
const fc::VtableLocation game_state_vtable{
    .rva = fc::Rva{0x00312000},
    .name = "GameStateVtable",
};

const auto update_slot = fc::vtable_slot<Update>(
    game_state_vtable,
    7,
    {.name = "UpdateGameStateSlot", .evidence = fc::points_to(original_update)});

plan.write(update_slot, &replacement_update);
```

The slot number is an index, not a byte offset. `vtable_slot` derives a typed data location using the correct pointer
size. Its `points_to` evidence checks the old function pointer before Commit replaces it.

## Give the game a larger table or buffer

Ask FusionCutter to allocate native storage when the game must point to data that does not exist in its image:

```cpp
const fc::DataLocation<std::uint32_t, 8> original_values{
    .rva = fc::Rva{0x00107000},
    .name = "OriginalValues",
};

const fc::DataLocation<std::uint32_t*> values_pointer{
    .rva = fc::Rva{0x00108000},
    .name = "ValuesPointer",
    .evidence = fc::points_to(original_values),
};

const auto values = plan.allocate_data<std::uint32_t>(16, "ExpandedValues");
plan.write(values_pointer, values.base());
```

`allocate_data` adds a storage request to the Plan. `values` is a symbolic handle because no native address exists yet;
`values.base()` lets another Plan operation refer to that future address without guessing it.

After Validation, FusionCutter creates and initializes the storage as part of Prepare. During Commit, it resolves the
symbolic address and writes the real pointer into `ValuesPointer`. If the patch's own runtime code also needs the
storage, retain its `DataHandle` in a handler and call `PrepareContext::resolve` from `prepare`.

See [Symbolic native data](SDK_REFERENCE.md#symbolic-native-data) for that complete handler pattern and for interior
addresses. The [Plan operations](SDK_REFERENCE.md#plan-operations) section lists every supported native change.

## Let the user choose a value

Define a normal C++ object for the setting, then describe how its member appears in the INI file. This example reuses
the `maximum_players` location defined above:

```cpp
struct PlayerLimitSettings {
    std::uint32_t maximum = 32;
};

[[nodiscard]] fc::Patch player_limit() {
    return fc::plan_patch(
        {
            .id = "PlayerLimit",
            .name = "Player limit",
            .settings = fc::settings<PlayerLimitSettings>(
                fc::value("Maximum", &PlayerLimitSettings::maximum, std::uint32_t{32})
                    .range(std::uint32_t{1}, std::uint32_t{64})
                    .description("Maximum number of players.")),
            .supports = {steam_client_game()},
        },
        [](fc::Plan& plan, const PlayerLimitSettings& settings) {
            plan.write(maximum_players, settings.maximum);
        });
}
```

Add `player_limit()` to the plugin's `.patches` list. FusionCutter generates and parses:

```ini
[General]
PlayerLimit=false

[PlayerLimit]
Maximum=32
```

FusionCutter generates the keys from the schema, parses the user's value, applies the range check, and creates the
completed `PlayerLimitSettings` object before the Plan stage. The Plan callback receives that typed object, so the
patch does not open the INI or convert text itself. Invalid settings fail before any native work is planned.

The SDK also supports strings, booleans, enum choices, sections, maps, environment overrides, and validation.

## Run code when the game calls a function

A checked write is finished once it has been installed. A hook is different: the game may enter your C++ callback at
any time afterward. FusionCutter therefore needs an object that remains alive with the callback and its state. The SDK
calls that object a handler.

A handler is an ordinary class. It does not inherit from a framework base class. This example counts calls to a game
function and shows the count in FusionCutter status:

```cpp
#include <atomic>
#include <cstdint>
#include <utility>

using Tick = void (*)() noexcept;

class TickCounter final {
  public:
    void plan(fc::Plan& plan) {
        plan.hook(
            fc::FunctionLocation<Tick>{
                .rva = fc::Rva{0x00234567},
                .name = "GameTick",
                .evidence = fc::exact_bytes(
                    {std::byte{0x55}, std::byte{0x8b}, std::byte{0xec}}),
            },
            [this](fc::Original<Tick> original) noexcept {
                calls_.fetch_add(1, std::memory_order_relaxed);
                original();
            });
    }

    void write_status(fc::StatusWriter& status) const noexcept {
        static_cast<void>(status.add("Calls observed", calls_.load(std::memory_order_relaxed)));
    }

  private:
    std::atomic<std::uint64_t> calls_ = 0;
};
```

The handler's `plan` method requests the hook during startup. The callback runs later whenever the game enters
`GameTick`; it increments the handler's atomic counter and calls the original game function. `write_status` reads that
counter when FusionCutter produces status output. The atomic keeps the value safe when the game and status writer use
different threads.

Register the handler as a patch:

```cpp
[[nodiscard]] fc::Patch tick_counter() {
    return fc::patch<TickCounter>({
        .id = "TickCounter",
        .name = "Tick counter",
        .supports = {steam_client_game()},
    });
}
```

Add `tick_counter()` to `.patches`. `fc::patch<TickCounter>` tells the SDK to use the handler form instead of the
concise `fc::plan_patch` form. FusionCutter creates one `TickCounter` for the selected support, calls its `plan` method,
and keeps the object alive while the patch is installed. That is why the callback can safely capture `this`.

`plan.hook` still follows the ordinary Plan path: it describes the function entry, expected bytes, callback, and hook
ownership without installing anything immediately. FusionCutter validates the site, prepares the hook privately, and
publishes it only after Commit succeeds. The handler does not need a `prepare` method merely because it owns a hook;
Prepare is for additional resources or lookups the handler itself needs.

`Original<Tick>` calls the game behavior that the hook replaced. A hook may instead skip the original or change its
arguments and result when the declared signature allows it.

Game threads may call the hook concurrently. Keep callbacks short and nonthrowing. Do not allocate, log, perform I/O,
take blocking locks, or do unbounded work inside them.

### When the handler needs more than a hook

The SDK recognizes lifecycle methods by their names and signatures; the handler does not inherit from a framework base
class. Add only the methods needed by the feature:

- `plan` describes native assumptions and coordinated work without changing the game;
- `prepare` resolves planned storage, finds required patch interfaces, or creates resources whose setup may fail;
- `activate` exposes already prepared behavior after Commit succeeds;
- `update` performs small, nonblocking housekeeping tasks on FusionCutter's serialized update pump; and
- `write_status` publishes a small current snapshot.

`prepare` returns `fc::Result`: an empty result means success, while `std::unexpected` wraps an `fc::Error` that
prevents Commit and explains the failure. For example, a patch that records frequent events can create a fixed-capacity
trace channel during preparation:

```cpp
fc::TraceChannel trace_;

[[nodiscard]] fc::Result prepare(fc::PrepareContext& context) {
    auto trace = context.create_trace({
        .name = "TickEvents",
        .capacity = 128,
        .max_record_size = sizeof(std::uint64_t),
    });
    if (!trace) {
        return std::unexpected(std::move(trace.error()));
    }
    trace_ = std::move(*trace);
    return {};
}
```

These methods go inside the handler class. `update` is not a game frame callback, and a persistent patch does not
imply that it needs a background thread. See [Handler lifecycle](SDK_REFERENCE.md#handler-lifecycle) for the method
signatures.

In this example, the trace channel remains private and inactive until installation succeeds. FusionCutter makes it
available before `activate` runs.

## Support another game version or server

The support block answers three questions:

- Which release layout is this implementation for?
- Does it run in the client, dedicated server, or both?
- Which executable image does it change?

The first patch supports `SteamRetail`, `Client`, and `Game`. Several layouts may share a support only when the same
handler and settings model implement them:

```cpp
.layouts = {fc::TargetLayout::SteamRetail, fc::TargetLayout::GOGRetail},
.roles = fc::HostRole::Client,
.image = fc::TargetImage::Game,
```

This does not make Steam addresses valid for GOG. FusionCutter calls each exact executable revision an image profile.
Use `plan.target()` to select a reviewed fact set for that profile. If none exists, fail the Plan instead of guessing.

A patch changes one executable image. Features spanning two images use two cooperating patches. See
[Target enums](SDK_REFERENCE.md#target-enums) and [Support definition](SDK_REFERENCE.md#support-definition).

## Work with another patch

Use relationships when one patch selects or requires another:

```cpp
.depends_on = {"RequiredTransport"},
.includes = {"OptionalDiagnostics"},
```

`depends_on` means the first patch cannot work without the named patch. `includes` requests a companion whose failure
does not invalidate the first patch. FusionCutter uses these relationships when selecting patches and orders required
providers so they finish installation before their consumers enter Prepare.

Use `Plan::observe` when another patch already owns a native hook and you only need notification. Compatible observers
share the same physical hook instead of installing another detour.

Use a patch interface for a small service, policy, or snapshot shared across plugins. See
[Patch interfaces](SDK_REFERENCE.md#patch-interfaces) once a feature needs that form of cooperation.

## Report what the patch is doing

FusionCutter already reports selection, installation, evidence failures, and conflicts. Add patch diagnostics for facts
specific to your feature:

- use debug logs for decisions, selected facts, and confirmation that runtime behavior first ran;
- use info logs for meaningful successful events;
- return an `Error` or use an error log when the intended behavior cannot continue;
- publish current counters, modes, and health through `write_status`; and
- use a trace channel for frequent structured events.

The Plan and each lifecycle context provide a patch-scoped logger. For example, a settings-aware Plan can record the
value it selected without repeating framework-owned installation messages:

```cpp
plan.logger().debug("Planning a maximum player count of {}", settings.maximum);
```

Do not log every frame, packet, or hook call. Keep current state in the handler and report it through status.

## Check the patch against a game image

`FusionCutter::Testing` loads the plugin through the normal startup path, reads its settings, selects its target
support, and checks its Plan without modifying the supplied file.

Add `tests/plugin_tests.cpp` and extend `CMakeLists.txt`:

```cmake
include(CTest)

if(BUILD_TESTING)
    fc_add_plugin_tests(ExamplePatchesTests
        PLUGINS
            ExamplePatches
        SOURCES
            tests/plugin_tests.cpp
    )
endif()
```

Create one scenario for each concrete image profile and role the patch supports:

```cpp
#include <FusionCutter/Testing.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>

TEST_CASE("example fix validates against the Steam client") {
    fc::test::Scenario scenario{fc::TargetLayout::SteamRetail, fc::HostRole::Client};
    scenario.add_plugin(std::filesystem::path{"plugins/ExamplePatches.dll"});
    scenario.add_image(
        fc::TargetImage::Game,
        "SteamRetail_Game_59EDE353",
        std::filesystem::path{"C:/private/game-images/BattlefrontII.exe"});

    const auto result = scenario.validate();
    REQUIRE(result.has_value());

    const auto* patch = result->find_patch("ExampleFix");
    REQUIRE(patch != nullptr);
    CHECK(patch->state == fc::test::PatchState::Ready);
}
```

The `Scenario` supplies the layout and role FusionCutter would receive from a loader. `add_plugin` admits the real
plugin DLL, while `add_image` pairs a reviewed executable file with its canonical image profile. `validate` then runs
Selection, Settings, Create, Plan, and common Validation without installing the patch. Looking up `ExampleFix` confirms
that this specific patch reached the expected `Ready` state.

Keep proprietary game images outside the repository. Supply the canonical image profile explicitly; the scenario does
not infer it from the file.

Scenario validation reaches the Plan but does not install hooks or run live callbacks. Add focused unit tests only when
the patch contains consequential C++ logic that image validation cannot exercise.

Build and run the tests with:

```powershell
cmake --build build/x86 --config RelWithDebInfo
ctest --test-dir build/x86 -C RelWithDebInfo --output-on-failure
```

## What else can you build?

The examples above cover common starting points. FusionCutter also supports patches that:

- check a function or value without changing it by adding a `Plan::require` operation;
- keep checked access to game data for runtime code with `Plan::require_mutable`;
- add a new function call or jump with `call_to` or `jump_to`;
- describe unusual ways a game function passes arguments and results with `NativeCall`;
- change behavior at a specific instruction and inspect the current machine state with a code hook and `CpuContext`;
- let several patches react to one hook through `Plan::observe` and ordered `before` or `after` callbacks;
- share a small service or piece of information between plugins through patch interfaces; and
- build richer configuration with `choice`, `section`, fixed `StringMap` entries, and settings validation.

This guide does not walk through every one of those cases. The [SDK reference](SDK_REFERENCE.md) explains the
FusionCutter tools behind them and provides focused examples.
