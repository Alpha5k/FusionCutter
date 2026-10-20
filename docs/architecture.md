# Architecture

Fusion Cutter runs inside the game process. Loaders start the runtime, plugins describe available patches,
and the runtime decides which patches can run. The SDK gives authors C++ interfaces for these declarations
and operations. A C application binary interface (ABI) defines the calls and data exchanged between external
plugin DLLs and the runtime.

## Components and source layout

| Directory | Responsibility |
| --- | --- |
| [loaders](../loaders) | DirectInput and Classic Collection proxies, dedicated server entry and common startup |
| [runtime/src](../runtime/src) | Configuration, plugin loading, patch selection, memory edits, hooks and shared services |
| [runtime/targets](../runtime/targets) | Game image profiles and implementations that read or modify particular game builds |
| [sdk/include/fc](../sdk/include/fc) | Public C++ interfaces and C ABI declarations |
| [sdk/example](../sdk/example) | An external plugin example |
| [plugins](../plugins) | The embedded Core plugin declaration |
| [patches](../patches) | Implementations of the bundled patches |
| [tools](../tools) | The offline verifier |
| [tests](../tests) | Automated checks and their fixtures |
| [cmake](../cmake) | Shared build configuration and packaging |
| [vendor](../vendor) | Pinned third-party dependencies |

The runtime separates image recognition in `images`, patch lifecycle work in `patches`, machine-code work in
`native`, and file output in `reporting`. Game-specific addresses and layouts belong in `runtime/targets` or
the patch that uses them. The build commands and component targets are in [CONTRIBUTING.md](../CONTRIBUTING.md).
The [SDK guides](sdk/README.md) describe the public interfaces and provide author examples.

## Startup

Client loaders load the adjacent `FusionCutter.dll` and call `FC_Start`. The dedicated loader contains the
runtime in the same DLL and starts it from a worker after DLL attachment. Startup establishes a fixed
Client or Server role; the current game session can change independently of that role.

The runtime then:

1. Locates its directory, initializes reporting and reads `FusionCutter.ini`.
2. Collects the embedded Core declaration and loads external DLLs directly from `plugins`.
3. Validates plugin declarations and ABI compatibility, then identifies the executable and relevant loaded DLLs.
4. Selects applicable variants, captures plugin configuration and resolves dependencies.
5. Installs available runtime game integrations and patch plans.
6. Continues checking for required DLLs and delivering updates on its control thread.

Missing required DLLs can become available later. Configuration is captured at startup so a patch that
installs later uses the same settings as the rest of that session.

## Plugins, patches and variants

A **plugin** is a declaration containing patches, groups and categories. An external plugin exports
`FC_GetPlugin`; the Core plugin is compiled into the runtime and goes through the same declaration checks.
Plugin IDs identify configuration files and qualify patch names, such as `Core.ColoredChats`.

A **patch** is a feature with an enabled default, optional configuration and dependencies. A **variant**
selects its handler and image requirements for particular builds and roles. Each required image has a label,
so a patch can operate on both the game and another module without confusing their addresses.

**Groups** collect patches for shared configuration and dependencies. **Categories** arrange patch and group
toggles into named INI sections.

An **image profile** identifies a known executable or DLL using image size and sections in its Portable
Executable (PE) headers. Catalog filenames help locate DLLs; the running executable is inspected directly,
including when it has been renamed. Profiles are catalog entries rather than a closed enumeration of
games. Variant selectors name profiles or
use a trailing `*` to match a profile prefix. These selectors choose from recognized profiles; each memory
edit or hook still needs the correct layout and evidence for its selected build.

Selection distinguishes three important outcomes:

- A matching variant can proceed once its dependencies are ready.
- A required DLL that has not loaded yet leaves the patch waiting.
- An incompatible executable, role or loaded image makes the variant unavailable. Patches for another
  executable are reported as not applicable instead of waiting for that executable to appear.

Ambiguous variants are rejected. Groups expand dependencies to all their members, and dependencies never
change a patch's enabled state. Configuration rules are described in [configuration](configuration.md).

## Patch installation

The runtime constructs a handler with its captured settings, then runs its lifecycle:

| Step | Responsibility |
| --- | --- |
| Plan | Describe memory reads, writes, hooks and allocations, with evidence for the game locations used |
| Validate | Check the plan and detect incompatible claims on game addresses |
| Prepare | Prepare memory, hooks and function handles, then let the handler initialize resources and register services |
| Install | Recheck the target state and apply the edits and hooks |
| Activate | Let the handler finish startup, then mark the patch installed |
| Update and Status | Run requested control work and collect any patch-specific status |

Ready patches install in dependency order. A provider completes installation and activation before a
consumer prepares. Unrelated patches use a stable order based on plugin and patch IDs. Runtime game
integrations install before public patches and participate in checks for conflicting edits and hooks.

Memory operations and hooks use an image plus a relative virtual address (RVA), the offset from that image's loaded
base. Plans validate expected bytes, instruction forms or referenced targets before changing them.
The runtime owns allocations, hooks and generated call bridges that adapt calling conventions. It also
checks whether operations can safely share a game address.

Compatible hooks from different patches share one installed hook at a game location. The hook registry
keeps the original code and a list of callbacks: observers run before and after one replacement callback,
or the original function when there is no replacement. Joining an existing hook updates that list.
Each invocation keeps the list it started with, so a patch installed during a call first participates in a
later call. The [shared hooks guide](sdk/native_hooks.md#shared-hooks) covers compatibility and callback order.

Preparation must make callback state usable before installation exposes a hook to the game. Installation
and activation are separate steps; the game can call an installed hook before activation finishes.

A failure before edits or hooks become active can reject the patch and release its resources. Its failure
policy decides whether the process may continue. A variant can override the patch's policy. Failures after
edits or hooks are exposed can require process termination because continuing would leave partially
initialized code running.

## Shared facilities

| Facility | Purpose |
| --- | --- |
| Game context | Report session and mission changes, including Local, Host, Client and Dedicated modes |
| Multiplayer | Track peers and negotiated features, and deliver patch messages |
| Lobby | Publish named lobby values and read cached or refreshed metadata |
| Patch APIs | Let patches publish interfaces and consumers retrieve them or receive availability callbacks |
| Logging and status | Explain events and failures, and summarize the current runtime and patch state |
| Tracing | Collect high-volume binary events through Event Tracing for Windows (ETW), with one file per session and channels owned by patches |

Game context, multiplayer and lobby are runtime services. Their public interfaces are shared across builds;
the implementations in `runtime/targets` supply the game's actual behavior. A service is available only when
the current build has a working implementation. See the [build support tables](installation.md#feature-support)
for current coverage.

Lua bindings are an embedded patch that publishes an API for callbacks and values in the game's Lua states.
Lua multiplayer builds on that API and the runtime's messaging service. Both use ordinary patch dependencies
and lifecycle operations.

## Threads and lifetime

The control thread serializes patch lifecycle callbacks, scheduled updates, status collection and delivery
of game, multiplayer and lobby notifications. Hook callbacks execute on the game thread that reaches
the hook. Authors must respect that distinction when sharing state.

Logging copies records to a reporting worker for file output. Tracing collects channel records separately
and writes ETW events. Status callbacks provide text on the control thread; the reporting worker writes the
assembled file. Callback work can therefore affect its calling thread even when file output is asynchronous.

Installed handlers, plugin modules, allocations and hooks remain available for the process lifetime. Failed
patches release resources that were never exposed. Restarting the process applies configuration or plugin
changes. Plugins share the game's address space and can still crash it through arbitrary code errors.

## Diagnostics and verification

Reporting keeps ordinary log messages, current status, crash information and high-volume tracing separate.
The crash reporter records exception context and known patch locations to help identify involved code.
See [troubleshooting](troubleshooting.md) for the files and how to interpret them.

`FusionCutterVerify.exe` reuses image recognition, selection and patch plan validation against files supplied
on the command line. It can find incompatible images and invalid plans before a game launch. It does not run
the full installed lifecycle, game callbacks or multiplayer sessions, so those behaviors still need live testing.
