# SDK documentation

Fusion Cutter runs C++ plugins inside Star Wars Battlefront games on Windows. An extension can change
game code or data, expose new Lua functions, exchange custom multiplayer messages, or provide a service for
other extensions. The runtime handles loading, compatibility checks, configuration, installation and reporting.

The SDK is the set of C++23 headers and build support used to write those extensions. You build a plugin DLL
and give it to players alongside Fusion Cutter. The runtime reads that plugin's declarations and runs the
patches that apply to the player's game. You can use existing runtime services without writing a loader or
finding game addresses yourself; direct changes to game code or data also require knowledge of the target binary.

These docs assume familiarity with C++ classes, functions and building a DLL, but no prior knowledge of Fusion
Cutter. The memory and hook guides explain FC's address and hook concepts; the Lua guides also assume basic Lua syntax.
For installing existing plugins without writing one, start with [installation](../installation.md).

## Reading the docs

1. Read [how plugins work](concepts.md) for the project's components, terminology and division of responsibilities.
2. Build [your first plugin](getting_started.md). It provides a visible result without needing any game addresses.
3. Follow [your first game patch](game_patch.md) to change a real instruction on Steam and GOG, verify the
   plan and check the installed result.
4. Read [declarations](declarations.md), [settings](settings.md) and [lifecycle](lifecycle.md) to understand how
   the runtime selects, configures and starts your code.
5. Choose the guide for the behavior you want to add. Each explains its purpose and examples before or alongside
   the API reference. You do not need to use every facility in one plugin.

For example, a game bug fix usually needs declarations, lifecycle, and memory edits or hooks. A mod adding Lua
functions can use the Lua helper directly. A networked feature also needs game context and multiplayer; a
diagnostic patch may need only observations and reporting. The C ABI guide is for understanding or adapting
the binary interface, and can be read after the C++ guides.

## Guides

| Guide | Covers |
| --- | --- |
| [How plugins work](concepts.md) | What FC provides, what authors build, and how the parts fit together |
| [Getting started](getting_started.md) | Build, export and install a complete plugin |
| [Your first game patch](game_patch.md) | Change the frame rate limit using real game locations, then verify and test it |
| [Declarations](declarations.md) | Plugins, patches, variants, image selectors, groups and categories |
| [Settings](settings.md) | Defaults, field bindings, enums, maps, validation and overrides |
| [Lifecycle](lifecycle.md) | Handler methods, contexts, failures, threads and lifetimes |
| [Game memory and edits](native_memory.md) | Locations, evidence, reads, writes, allocations and branches |
| [Game functions and hooks](native_hooks.md) | Calling conventions, originals, shared hooks, observers, registers and expected faults |
| [Patch APIs](patch_apis.md) | Shared contracts, providers, consumers and callbacks |
| [Game context](game.md) | Session identity, modes and notifications |
| [Multiplayer](multiplayer.md) | Peers, feature agreement, messages and broadcast |
| [Lobby](lobby.md) | Named lobby values, publication, cached reads and refresh requests |
| [Lua](lua.md) | Bindings, globals, values, calls, references and work on a Lua thread |
| [Lua multiplayer](lua_multiplayer.md) | Script channels, peers and message delivery |
| [Reporting](reporting.md) | Logging, status, trace channels and fatal errors |
| [Verification and debugging](verification.md) | Build checks, offline verification, live testing and callback debugging |
| [C ABI](abi.md) | Export rules, table layout, ownership and the C header reference |

The [architecture guide](../architecture.md) explains runtime internals. Player-facing configuration and
installation instructions are in [configuration](../configuration.md) and [installation](../installation.md).

## Terms used in the API

| Term | Meaning |
| --- | --- |
| Plugin | A declaration containing patches, exported from a DLL or embedded in the runtime |
| Patch | A feature with controls, dependencies and one or more variants |
| Variant | A handler implementation selected by required images and launch role |
| Handler | The C++ object that implements a selected patch's lifecycle |
| Image | A loaded executable or DLL |
| Profile | A catalog entry identifying one supported image build |
| Image label | A variant-local name, such as `game`, used to choose an image within a plan |
| RVA | Relative virtual address: a byte offset from an image's loaded base |
| Evidence | Expected bytes, instruction form or destination used to validate a game address |
| Plan | The memory operations, hooks and resources a patch requests before installation |
| Control thread | The runtime thread that serializes lifecycle work and service notifications |
| Borrowed value | A view or pointer whose storage belongs to another object and has a stated lifetime |
| ABI | Application binary interface: the calls and layouts exchanged between separately compiled DLLs |

## Public headers

| Include | Public surface |
| --- | --- |
| `<fc/plugin.hpp>` | `Plugin`, `Patch`, `Variant`, `variant`, `ImageRequirement`, `Category`, `Group`, `Role`, `FailurePolicy`, `FC_PLUGIN` |
| `<fc/base.hpp>` | `Error`, `Result<T>`, `Bytes` |
| `<fc/settings.hpp>` | `setting`, `settings`, `SettingBuilder`, `Setting`, `TypedSettingsSchema`, `SettingsSchema`, `StringMap` |
| `<fc/contexts.hpp>` | `CreateContext`, `PrepareContext`, `ActivateContext`, `UpdateContext`, `StatusContext` |
| `<fc/native.hpp>` | `Plan`, game addresses and evidence, resource handles, hooks, call layouts and helpers |
| `<fc/patch_api.hpp>` | `Function`, `bind`, `ConsumerCallback`, `CallbackError`, `ApiContract`, `api`, `RecordDescription`, `describe`, `TextView`, `ByteView` |
| `<fc/game.hpp>` | `Game`, `GameState`, `GameMode`, `SessionId` |
| `<fc/multiplayer.hpp>` | `Multiplayer`, `Feature`, feature definitions and outcomes, `PeerId`, `PeerInfo` |
| `<fc/lobby.hpp>` | `Lobby`, `LobbyItem`, `Request`, `LobbyId`, `LobbyRecord`, `LobbyResult` |
| `<fc/lua.hpp>` | `fc::lua` bindings, frames, values, references and result types |
| `<fc/reporting.hpp>` | `Logger`, `LogLevel`, `TraceDefinition`, `TraceChannel`, `TraceHealth`, `fatal` |
| `<fc/abi.h>` | Runtime and plugin C records and function tables |

Use the public headers above. Types in `fc::detail` implement adaptation and storage; they are not the
author interface. `fc::abi` is different: it contains the public descriptions of game function calling conventions.

Examples that accept a game address assume you supply a layout verified for the selected build. They
demonstrate API use without assigning invented game addresses. Complete plugin examples can be built;
smaller snippets belong inside the surrounding handler or helper described in their guide.
