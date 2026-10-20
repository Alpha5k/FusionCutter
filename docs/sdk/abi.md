# C ABI reference

The C++ SDK owns declarations, typed settings and callback captures inside the plugin DLL. It lowers them
to C records for the runtime. Most plugin authors should use the C++ interfaces; this guide describes the
boundary for adapters, contract headers and low-level diagnosis.

## Why a binary interface is needed

A plugin DLL and the runtime are compiled separately. The runtime cannot inspect an arbitrary C++ handler
class or assume that both modules use identical C++ library objects. An **application binary interface**, or
ABI, specifies the function calls and memory layouts they can exchange.

FC uses C records, explicit lengths and function pointers at that boundary. The SDK translates an author
declaration such as `fc::Patch` into those records and supplies callbacks that invoke the handler inside its
own DLL. The runtime schedules the callback, while the SDK's code knows the concrete C++ handler type.
This is why plugin authors can use ordinary classes and typed settings without exposing their internal
layout as the runtime's interface.

For example, a description can be owned by a `std::string` in the plugin while an `FC_Text` points to its
bytes for the runtime. The string object and its allocator stay in the plugin; the runtime sees a defined
record containing a pointer and length. If the runtime needs the text after the agreed borrowing period, it must copy it.

The normal C++ workflow already performs this adaptation. Read this reference when writing an adapter,
investigating an ABI rejection or checking ownership in a shared contract. The [first plugin](getting_started.md)
requires no hand-written C tables.

## Plugin loading

FC examines every DLL directly inside `plugins` beside the runtime and expects each one to export
`FC_GetPlugin`. It does not search subdirectories.

FC loads plugins with `LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32`. When Windows
needs to search directories for an imported DLL, these flags select the plugin's directory and System32.
The game executable's directory and subdirectories of `plugins` are not included in that search.

A library DLL beside your plugin can satisfy an import, but FC will also try to load it as a plugin and
report a missing `FC_GetPlugin` export. Use static linking where suitable. A separate library in a subdirectory
requires explicit loading by your plugin; moving an ordinarily imported DLL there alone prevents loading.

## Version and export

`FC_ABI_VERSION` in [common.h](../../sdk/include/fc/abi/common.h) is currently `2u`. This is independent of
the Fusion Cutter distribution version or a plugin's displayed version. The runtime and plugin must agree
before the runtime reads `FC_Plugin`.

```cpp
extern "C" FC_EXPORT std::uint32_t FC_CDECL FC_GetPlugin(
    std::uint32_t abi, const FC_Plugin **out, FC_ErrorSink error);
```

The export must be undecorated on both architectures. Success returns exactly 1 and a declaration whose
records, arrays and text remain immutable while the plugin is loaded. Failure returns 0, clears a supplied
output pointer and reports synchronously through the optional error sink. `FC_PLUGIN(factory)` supplies this
implementation, stable declaration storage and x86 export handling.

An undecorated export means the DLL exposes the exact name `FC_GetPlugin`. C++ compilers can otherwise encode
type or calling-convention details in symbol names. FC looks up the agreed name, so a function with a similar
source spelling but a different exported name is not the entry point.

There are several independent compatibility names in a plugin:

| Value | What it describes |
| --- | --- |
| `FC_ABI_VERSION` | Runtime/plugin declaration and service boundary |
| `Plugin::version` | Release displayed to users and in diagnostics |
| Patch API contract ID and layout | Callable behavior shared by particular patches |
| Multiplayer feature ID | Protocol support advertised to another process |

Changing the displayed plugin version does not negotiate a new API layout or network protocol. Use the
relevant mechanism for the boundary that actually changed.

## Common boundary rules

Every pointer at this boundary needs an owner and a valid lifetime. The common records make storage visible
without transferring arbitrary C++ ownership. A service table is a record of function pointers supplied by
the runtime; an opaque handle identifies a runtime object without exposing its internal fields.

| Representation | Contract |
| --- | --- |
| `FC_Text` | Borrowed UTF-8 pointer and uint32 byte count; no terminator required |
| `FC_Bytes` | Borrowed byte pointer and uint32 count |
| Pointer/count pairs | Null pointer permitted only for zero count |
| `FC_ErrorSink` | Context and optional synchronous write callback; copy text before returning |
| `FC_Host` | Stable service table pointers and readable prefix size |
| `byte_size` | Verify a field fits in the published prefix before accessing it |
| Boolean fields | uint32 values 0 or 1 where specified |
| Opaque runtime handles | Runtime-owned identity; use the corresponding service operations |

No exception may cross a C callback. Allocate and free author-owned objects in the same DLL. Calls generally
borrow input until return unless their contract explicitly copies or retains it. A null host service table
means unavailable. Availability of a table does not by itself mean the current game build has an active backend.

`FC_CDECL` establishes the boundary calling convention and `FC_EXPORT` the export attribute. Public headers
are usable from C; the C++ wrappers add typed ownership and error results without moving STL objects across DLLs.

`byte_size` describes how much of an extensible table a caller can read. A field is usable only if the table's
published size includes it and its operation is present. This allows callers to check optional service entries
without reading beyond the published record. It does not override the top-level ABI version check or turn
an unavailable game backend into an available one.

The C ABI described here is also separate from a game function's calling convention. The runtime and plugins
use the agreed FC boundary; [game function call layouts](native_hooks.md#explicit-call-layouts) describe the registers
and stack used when invoking or hooking game code.

## Declarations and lifecycle

[plugin.h](../../sdk/include/fc/abi/plugin.h) defines:

| Record | Role |
| --- | --- |
| `FC_Plugin` | Prefix size, identity/version and category/group/patch arrays |
| `FC_Category` | ID plus explicit presence/value for optional order |
| `FC_Group` | Local member IDs and enabled/configurable controls |
| `FC_Patch` | Controls, default failure policy, update interval, dependencies and variants |
| `FC_ImageRequirement` | Variant-local image name and profile selectors |
| `FC_Variant` | Images, role, effective failure policy, additional dependencies, effective settings and factory |
| `FC_Setting` | Scalar default, presentation metadata and validation callback |
| `FC_Value` | Tagged bool, signed, unsigned, real or text scalar |
| `FC_HandlerFactory` | Create a handler from values in schema order, with stable owner/host access |
| `FC_Handler` | Opaque object and destroy, plan, prepare, activate, update and status callbacks |
| `FC_StatusWriter` | Callback-scoped line writer |

The SDK resolves inherited settings and failure policies before publishing FC_Variant. It retains typed
completion validation and handler construction within the plugin. C factories receive captured scalar values
only for the creation call and must copy retained data. Destruction returns to the allocating DLL.

Plan and Prepare return 1 or 0 with an optional error. Optional lifecycle callbacks are null. Update receives
steady-clock `now_ns` and `elapsed_ns` in nanoseconds. Status receives a temporary writer. Follow the same
[lifecycle and thread rules](lifecycle.md) as the C++ interface.

## Service headers

| Header | Records and operations |
| --- | --- |
| [native.h](../../sdk/include/fc/abi/native.h) | Locations, evidence, address descriptions, call layouts, CPU contexts, operation union, FC_Plan and FC_NativeApi |
| [abi.h](../../sdk/include/fc/abi.h) | FC_ApiContract, FC_PatchApi publication/get/notification/invocation, plus runtime startup types |
| [game.h](../../sdk/include/fc/abi/game.h) | FC_GameState, current snapshot and change registration |
| [multiplayer.h](../../sdk/include/fc/abi/multiplayer.h) | Peer information, feature registration, agreement, message submission and callbacks |
| [lobby.h](../../sdk/include/fc/abi/lobby.h) | Item publication, current lobby, cached/async reads and cancellation |
| [reporting.h](../../sdk/include/fc/abi/reporting.h) | Log packets/formatters, trace definitions/health and fatal reporting |
| [lua.h](../../sdk/include/fc/abi/lua.h) | Lua invocation, value, range, reference and iterator records used by the patch API |

`FC_Plan::submit` copies operation descriptions and returns any requested resource. `read` copies a planning
snapshot; the image array names selected requirement labels and profiles. `FC_NativeApi` provides prepared
data, memory copies/views, game function address resolution and calls, expected faults and call return addresses.

Patch API contracts carry an exact ID, table size/alignment, description bytes and required function offsets.
Publication copies the table. Required retrieval copies a compatible installed provider table; optional
notification borrows retained table storage. `invoke` checks a consumer's activity before calling its thunk.

Game and message callbacks borrow incoming records until return. Lobby refreshes retain the callback context
until `cancel`; raw C callers must cancel every accepted request exactly once, including after completion.
The C++ Request wrapper performs that release. Log packet formatters run on the reporting worker, so their
module and code must remain loaded. Trace records are copied on submission.

The Lua provider's complete function-table contract is assembled in
[lua/contract.hpp](../../sdk/include/fc/lua/contract.hpp). Use the public Lua helper for ordinary consumers;
the raw records still obey Lua-thread and invocation-lifetime restrictions.

For field layouts, enum numeric values and exact C signatures, use the linked headers. The corresponding
SDK guides describe each operation's behavior without duplicating binary layouts in prose.
