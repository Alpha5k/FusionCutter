# How Fusion Cutter plugins work

Fusion Cutter lets C++ code extend a running Battlefront game. It can replace a value in game memory,
intercept a function call, expose a function to Lua, or exchange data with another player's extension. The
same runtime provides configuration and diagnostic output for all loaded plugins.

A runtime patch changes the game process in memory. Fusion Cutter applies it again on each launch; the
patch does not need to rewrite the executable on disk. Game assets and scripts remain separate files when
a mod needs them. A plugin may work with those assets, but installing its DLL does not install a map or a
`.lvl` file automatically.

## What you build and what the player installs

There are three pieces involved in running an external plugin:

| Piece | Responsibility |
| --- | --- |
| Loader | Starts Fusion Cutter inside the game or dedicated server process |
| Runtime | Loads plugin DLLs, selects applicable patches, installs changes and provides shared services |
| Plugin | Declares features and contains the author's implementations of them |

The SDK is used while building the plugin. Its headers turn C++ declarations and callbacks into the binary
interface the runtime understands. Players run your compiled DLL; they do not need the SDK or a compiler.
The runtime includes a Core plugin with bundled fixes and Lua facilities. External plugins use the same
patch model as Core.

Choose an existing loader using the [installation guide](../installation.md). For normal plugin development,
build the plugin for the game's architecture and place it in the runtime's `plugins` directory. The runtime
loads it on the next launch. Configuration and code changes take effect after restarting the game.

## One plugin can contain several patches

A plugin is the distribution and naming unit. A patch is one feature the runtime selects, configures and
starts. For example, a plugin could contain a chat display patch and a separate network diagnostics patch.
They can have independent toggles and support different game builds while shipping in the same DLL.

Each patch declares one or more **variants**. A variant says which executable or DLL builds and launch roles
its implementation supports. The implementation is a **handler**, a C++ object whose methods the runtime
calls during startup and operation.

```text
Plugin: Example
  Patch: ChatDisplay
    Variant for supported Steam/GOG clients -> ChatHandler
  Patch: NetworkDiagnostics
    Variant for supported clients/servers -> DiagnosticsHandler
```

The runtime chooses one applicable variant for each patch. One handler may support several builds by selecting
the right address layout. A substantially different implementation can use another handler. Neither choice
requires presenting several copies of the same feature to the player.

Groups and categories organize patches for different reasons. A group can supply a shared enabled toggle
and can be depended on as a whole. A category places controls under an INI heading. They are optional;
the [first plugin](getting_started.md) needs neither.

## Compatibility has several parts

The game executable and its DLLs are called **images** when discussing memory operations and hooks. The runtime's
image catalog identifies recognized builds with **profile IDs**, such as `BF2.Steam.59EDE353`. A variant
selects the profiles it supports. This lets the runtime exclude a patch for another game before running its
handler or adding its settings to configuration.

Recognizing a build answers which image is loaded. An edit or hook also needs the correct address, type
or instruction for that build. Authors provide that layout and can attach **evidence**: the bytes, value or
destination they expect at a location. The runtime checks that evidence before applying the operation.

Shared services have their own build coverage. A recognized Battlefront I build can run applicable patches
without having the Battlefront II Galaxy messaging implementation. A compatible game may also be sitting
at a menu with no network session. Check the service's availability as well as declaring suitable variants.
The [build support tables](../installation.md#feature-support) distinguish image recognition from services.

## From declaration to running code

On startup, the runtime reads the plugin declaration, identifies applicable variants, captures configuration
and checks dependencies. Only enabled, applicable patches can proceed. A dependency requires another patch
to be available and installed; it does not enable that patch for the player.

The runtime then constructs the selected handler and gives it the stages it implements:

1. **Plan:** describe edits and hooks and reserve resources, such as an original function handle or replacement data.
2. **Prepare:** initialize those resources and register callbacks or shared APIs before changes become reachable.
3. **Install:** the runtime applies the planned edits and hooks.
4. **Activate:** finish startup. Later Update and Status callbacks run if the handler defines them.

These stages keep installation work out of callbacks that run repeatedly during gameplay. A patch using only
a shared service may need Prepare and no Plan at all. A simple constant edit may need Plan and no other
methods. The SDK detects the methods present; there is no base class requiring empty implementations.

A **context** passed to a handler method gives access to that stage's operations. It lasts for the method
call. Plan supplies memory operations and hooks. A **handle**, such as `Logger`, `Game` or `Feature`, refers
to a service or resource and can be retained according to its contract. The [lifecycle guide](lifecycle.md)
explains these lifetimes and threads.

## Choose the facility that matches the job

| Goal | Facility | What it contributes |
| --- | --- | --- |
| Change a constant, pointer or instruction | [Game memory and edits](native_memory.md) | Describes locations, checks evidence and applies planned changes |
| Call, replace or inspect game behavior | [Game functions and hooks](native_hooks.md) | Adapts game function calls and runs callbacks where the game executes |
| Reuse functionality from another patch | [Patch APIs](patch_apis.md) | Connects a provider and consumer inside the same process |
| React to joining, hosting or leaving a session | [Game context](game.md) | Reports session identity and current participation |
| Exchange custom data between connected players | [Multiplayer](multiplayer.md) | Checks matching feature support and transports messages |
| Attach a value to a hosted lobby | [Lobby](lobby.md) | Publishes named metadata and reads current or cached records |
| Make C++ functionality callable by game scripts | [Lua bindings](lua.md) | Registers functions and handles Lua values and VM lifetimes |
| Exchange Lua values between game processes | [Lua multiplayer](lua_multiplayer.md) | Routes serialized values to named script listeners |
| Explain failures or inspect behavior | [Reporting](reporting.md) | Supplies logs, current status and binary trace records |

A patch API call stays inside one game process. A multiplayer message goes to a peer in the session. A Lua
binding connects scripts to C++ within a Lua VM. A feature can combine these: a C++ provider might expose a
Lua function whose implementation sends a multiplayer message, then updates a local status counter.

## The author still defines the behavior

Fusion Cutter manages declared operations and resources. It does not decide what a patch's protocol means,
which game function is safe to call at a particular moment, or whether a value supplied by another player
is valid for the game rule being implemented. Those decisions belong in the patch.

Plugins execute inside the game process. Compatibility checks can catch a wrong image, unexpected
instruction or conflicting operation; they cannot make arbitrary C++ code safe. Use the existing services
where they provide the behavior you need. Check the relevant game code and data before adding memory edits or hooks.

For a new feature, first identify its player-visible behavior and supported builds. Then choose the services,
declarations and handler methods needed to implement it. Start with [your first plugin](getting_started.md)
to see that path through a complete, buildable example.
