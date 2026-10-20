# Handler lifecycle, threads and lifetime

A handler is the object that owns one installed patch's state: configured values, resource handles and data
used by callbacks. FC creates it after selecting the patch's variant and calls its methods at defined stages.
The **lifecycle** is that sequence of construction, installation and later callbacks.

## Why work is split into stages

Installing a hook may require an original function handle, replacement data and a connection to another patch.
Those things need to be ready when the game first reaches the hook. Plan describes the required edits, hooks
and resources. Prepare initializes the resulting resources and connects services; installation makes the changes reachable.
This also gives FC a chance to reject an invalid plan before exposing it to the game.

Stages separate work that happens once from work that happens repeatedly. Register a message callback in
Prepare; process received messages in that callback. Reserve a hook in Plan; run its behavior when the game
calls it. An Update method is only needed for work that should recur on FC's control thread.

Implement only the methods the handler needs; the SDK detects them and constructs the C handler table.
The following is a signature reference, not a requirement to define every method:

```cpp
class Handler {
  public:
    Handler(const fc::CreateContext &context);
    void plan(fc::Plan &plan);
    fc::Result<> prepare(fc::PrepareContext &context);
    void activate(fc::ActivateContext &context) noexcept;
    void update(const fc::UpdateContext &context) noexcept;
    void write_status(fc::StatusContext &context) const noexcept;
};
```

A handler without settings may instead use a default constructor. A handler with settings declares
`using Settings = YourSettings` and accepts `(const fc::CreateContext &, const Settings &)`. Copy the
values it needs; the constructor's settings reference expires when construction returns.

## A patch through startup

Consider a patch that replaces a table in game memory and reports how many entries it contains. Its constructor
copies the configured size. Plan reserves the replacement array and describes the pointer and count edits.
Prepare fills the array while it is private to the patch. The runtime installs the edits so the game can use
the array. Status later reports the size retained by the handler.

That patch needs no Update if the array never changes. A patch that registers a Lua function instead may
need only Prepare: the Lua provider already installs the required game hooks. The handler's methods follow its
actual responsibilities, not a fixed template of empty stages.

## What each stage provides

| Stage | Access and responsibilities |
| --- | --- |
| Construction | `CreateContext::log()`; retain configured author state |
| Plan | `Plan` image views, memory operations, resources, hooks and `log()` |
| Prepare | Prepared allocations, hooks and function handles; `game()`, `multiplayer()`, `lobby()`, `trace()`, `publish()`, `get()`, `on_available()`, `callback()`, `log()` |
| Install | Runtime applies the plan; no author callback |
| Activate | `ActivateContext::log()`; complete startup after installation |
| Update | `UpdateContext::now`, `elapsed`, `log()`; requested periodic work |
| Status | `StatusContext::line()` and `log()`; describe current state |

All these callbacks run on the runtime's control thread. Context objects, `Plan` and its copied views
expire at the end of their stage call. Retain service handles, loggers and resource handles obtained from
them rather than the context object itself.

For example, `context.log()` returns a Logger that a later hook can use. Retaining `&context` in that hook
would instead refer to the completed Prepare call. The same distinction applies to Plan: submit operations
while Plan is valid, then keep their resource handles rather than trying to submit another installation later.

`game()`, `multiplayer()` and `lobby()` return service handles directly. Obtaining one does not establish
that the current build or session supports an operation; inspect the query or operation's result.
`trace(definition)` returns `Result<TraceChannel>` because it creates a channel during Prepare.
The [patch API guide](patch_apis.md#prepare-method-reference) lists the return types and errors for
`publish`, `get`, `on_available` and `callback`.

The runtime validates plans, prepares resources and calls `prepare`. It then rechecks the game code and data
used by the plan, installs changes and calls `activate`. Dependencies complete installation and activation
before a consumer prepares.
After activation, the runtime publishes the handler as installed and delivers registered notifications.

An installed hook can execute before `activate` finishes. Initialize everything it needs in construction
or Prepare. There is no global pause between installing edits or hooks and activation.

## Errors and failure policy

`fc::Result<T>` is `std::expected<T, fc::Error>`. `Result<>` has no success value. `Error` owns `message` and
an optional `windows_error` code, with zero meaning that no Windows error applies.

Test a Result before reading its value. `if (result)` means success, `*result` accesses the successful value,
and `result.error()` accesses the failure. `return {}` succeeds for `Result<>`; `std::unexpected(error)`
returns a failure. This lets a handler pass a service's error back to FC without inventing a second message:

```cpp
fc::Result<> prepare(fc::PrepareContext &context) {
    auto channel = context.trace({"Samples", 1, 64 * 1024});
    if (!channel) {
        return std::unexpected(channel.error());
    }
    trace_ = *channel;
    return {};
}
```

Construction and preparation can report normal failures. The SDK converts construction, planning and
preparation exceptions into startup errors. `plan` returns void: use `plan.fail(message)` for a decision
that invalidates the plan. Failed plan operations remain recorded even if an individual result is ignored.

`activate`, `update`, `write_status`, hook callbacks and ordinary runtime notifications must be `noexcept`.
Handle expected errors explicitly. An unexpected exception or access violation during a live callback is
fatal; a `Continue` startup policy does not turn it into a recoverable callback result.

Result describes an operation's expected failures, not every possible C++ exception. SDK operations that
allocate storage can still throw, for example while copying a vector. The startup adapter catches exceptions
from construction, Plan and Prepare. A live `noexcept` callback must handle any recoverable exception itself.

Before edits or hooks are exposed, a rejected handler is destroyed and its reservations are discarded.
Once changes are exposed, failure may terminate the process to avoid continuing with incomplete state.
Installed handlers and their modules remain for the process lifetime. Do not rely on a normal shutdown
destructor to save essential state or remove installed hooks.

## Updates and status

`Patch::update_interval` is a positive `std::chrono::milliseconds` duration, defaulting to 50 ms. Only a
handler with `update` needs periodic callbacks. A delayed update runs once with its actual elapsed time;
it is not a fixed-rate simulation clock.

`UpdateContext::now` is a steady-clock sample. `elapsed` is the time since activation for the first update,
then since the preceding update. Store these as steady-clock values, not wall-clock timestamps.

`write_status` is const and supplies complete lines through `StatusContext::line(text)` or its formatting
overload. The writer expires on return. Return values indicate whether a line fit; avoid doing expensive
work just to display status. See [reporting](reporting.md).

## Thread and ownership reference

The control thread is FC's own execution thread for lifecycle work and service notifications. A hook
runs on whichever game thread reaches it. Lua work runs on the thread using that Lua VM. Receiving a context
or service handle does not move the calling code to another thread.

This matters when, for example, a hook increments a counter and Status reads it. Those callbacks can overlap.
Protect the shared counter appropriately. Two callbacks that both run only on the control thread are serialized
and do not need synchronization merely because they are different methods.

| Object or operation | Thread and lifetime |
| --- | --- |
| Lifecycle methods | Serialized on the control thread |
| Hooks and game function calls | The thread reaching the hook or making the synchronous call |
| `Game::current()` | Any thread; returns a copied snapshot |
| Game, feature and optional API notifications | Control thread, after activation |
| Multiplayer registration and `clients()` / `broadcast()` | Control thread; register features and callbacks during Prepare |
| Multiplayer `host()`, `local()`, `info()`, feature `agreement()`, `send()`, `max_payload()`, `fail()` | May be used from other threads; only send promises immediate acceptance or refusal without waiting for the service lock |
| Lobby operations and `Request` destruction | Control thread |
| Lua `Frame`, `Value`, `Results` | Owning Lua thread and current invocation only |
| Lua state notifications and capture cleanup | Control thread |
| `Function` and `ConsumerCallback` calls | Synchronous on the caller's thread; the provider defines permitted use |
| `Logger`, `TraceChannel` queries and writes | Any thread; writes copy into runtime queues and file work is asynchronous |
| `Memory::view()` | View of game memory or a runtime allocation; author manages synchronization and object lifetime |

Copies of most handles refer to the same runtime resource. They do not copy or extend the lifetime of a
game object or connection. `Request` and `lua::FunctionRef` are move-only owners with explicit cancellation
or release behavior. Borrowed callback arguments must be copied before retaining them.

Here **borrowed** means that another object owns the storage. A `std::string_view`, span or callback argument
can refer to valid data during a call and become invalid afterward. Copying the view copies its pointer and
length, not the underlying data. In contrast, copying received bytes into your own vector preserves those
bytes after the callback returns. Individual guides identify which values are borrowed and which own storage.

Control-thread serialization does not synchronize state with hook or Lua callbacks. Use suitable atomics
or other synchronization for state shared across those threads, and keep work inside game callbacks short.
