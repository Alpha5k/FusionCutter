# Lua bindings and values

The game runs Lua scripts in an embedded interpreter. A **binding** exposes C++ behavior as a Lua
function, letting a script call an operation that the game's existing Lua interface does not provide.
FC's Lua helper also lets a patch read or set globals, call Lua functions and work with Lua values.

For example, a patch can expose `Example.GetStatistics()` for a mod's script, add `Example.version` so a
script can identify the extension, or wrap an existing function while preserving its original behavior.
The Core provider handles integration with the game's Lua implementation. Consumers use the SDK helper
without managing the raw Lua stack or repeating the game hooks.

Include `<fc/lua.hpp>` and declare a dependency on `core.lua_bindings`. Retrieve its SDK helper with
`fc::lua::get(context)` during Prepare. See the Lua bindings row in the
[build support tables](../installation.md#feature-support) for the provider's supported builds.

## States, frames and values

A Lua **VM**, or virtual machine, is an interpreter instance with its own globals and objects. The game
can create and destroy these instances while the FC patch remains installed. Registering a binding once
lets the provider apply it to current and future supported VMs, instead of making each patch track creation
and repeat its registration.

`StateId` identifies an actual VM lifetime. A state is not limited to a named shell or mission role;
coroutines share their VM's identity. Setting a global in one VM does not set it in every other VM.
Use `initialize` for setup that should happen in each VM and a specific StateId for work aimed at one VM.

A `Frame` describes one invocation into Lua through this API. It gives the callback access to incoming
arguments, globals and return values. A `Value` refers to a Lua value within that invocation. Here "frame"
means a call's working context, not a rendered game frame. The frame and its Value wrappers expire when the
callback returns, even if the actual Lua object remains stored in a global or table.

## A complete binding patch

This plugin creates `Example.version` and a callable `Example.Add(a, b)` in each supported VM:

```cpp
#include <fc/plugin.hpp>
#include <fc/lua.hpp>

namespace example {

class Handler {
  public:
    // Installs a global table and one function in current and future Lua states.
    fc::Result<> prepare(fc::PrepareContext &context) {
        auto bindings = fc::lua::get(context);
        if (!bindings) {
            return std::unexpected(bindings.error());
        }
        auto initialized = bindings->initialize([](fc::lua::Frame &frame) {
            auto table = frame.global("Example");
            if (table.type() == fc::lua::Type::Nil) {
                table = frame.table();
                frame.global("Example", table);
            }
            if (table.type() != fc::lua::Type::Table) {
                frame.error("Example must be a table");
                return;
            }
            table.set("version", frame.value("1"));
        });
        if (!initialized) {
            return initialized;
        }
        return bindings->bind({"Example", "Add"}, [](fc::lua::Frame &frame) {
            auto a = frame.argument(0).as_number();
            auto b = frame.argument(1).as_number();
            if (!a || !b) {
                frame.error("Add expects two numbers");
                return;
            }
            frame.return_values({frame.value(*a + *b)});
        });
    }
};

// Declares the binding patch for builds supported by the Lua provider.
fc::Plugin build_plugin() {
    return {
        .id = "ExampleLua",
        .version = "1",
        .patches = {fc::Patch{.id = "Bindings",
                              .enabled = true,
                              .depends_on = {"core.lua_bindings"},
                              .variants = {fc::variant<Handler>({"BF2.Steam.59EDE353", "BF2.GOG.59EDF52B", "BF2.ModTools.43EBD102"})}}}};
}

} // namespace example

FC_PLUGIN(example::build_plugin)
```

Lua usage after the bindings have installed:

```lua
local total = Example.Add(2, 3)
print(Example.version, total)
```

The callbacks and their captures are retained by the provider; the local Bindings wrapper does not have to
survive Prepare. Retain a Bindings handle when you also need `run` or `defer` later.

The dependency ensures Core's provider is installed before this handler prepares. `fc::lua::get` retrieves
that provider's API and wraps it in the typed helper. `initialize` then declares the setup to run for each VM:
find or create the `Example` table, check its type and set its version field.

`bind({"Example", "Add"}, ...)` declares a function under that table. When a script calls `Example.Add(2, 3)`,
the callback receives a Frame containing two arguments. It converts them to numbers, adds them and selects
one return value. The Lua snippet should therefore print version `1` and total `5`. Passing text instead of
a number produces the declared Lua error after the C++ callback returns.

The C++ plugin supplies these globals; a mod must still arrange for its Lua usage code to run through the
game's script loading. FC does not automatically load an accompanying script file. Scripts that can run
without this plugin should check for the table/function before calling it.

## Registering functions and observing states

| `Bindings` method | Contract |
| --- | --- |
| `initialize(callback)` | `Result<>`; register in Prepare to run once per VM before this patch's bindings install |
| `bind(name, callback)` | `Result<>`; add a literal global name |
| `bind({path...}, callback)` | `Result<>`; add a function under literal table keys |
| `bind(name_or_path, mode, callback)` | `Result<>`; explicit Add, Wrap or Replace |
| `on_state(callback)` | `Result<>`; register in Prepare to receive `(StateId, bool alive)` on the control thread |
| `run(state, callback)` | `Result<>`; run synchronously on the calling thread, which must already be that VM's Lua thread |
| `defer(state, callback)` | DispatchResult; queue work for that VM's Lua thread during game updates |

Frame callbacks accept `Frame &`. They run on the Lua thread. State notifications report existing VMs after
activation and subsequent lifetime changes on the control thread. Synchronize data shared between those
callbacks. Accepted callback captures are destroyed on the control thread.

`fc::lua::get(context)` returns `Result<Bindings>` and uses the required patch API connection to
`core.lua_bindings`. A default Bindings object has no provider: registration and `run` return errors, and
`defer` returns Unavailable. Copies of a valid Bindings object use the same provider and consumer identity.

StateId contains `uint64_t value` and supports equality. Zero means absent. It can be retained after a
notification, but the VM may end before later work executes. `run` rejects an ended or unknown VM, a call
from another thread, or an inactive consumer; Lua failures are also returned in its Result.

`bind("Example.Add", callback)` uses one global whose name contains a dot.
`bind({"Example", "Add"}, callback)` uses a table path. Each path element is one literal key.

| `BindingMode` | Behavior |
| --- | --- |
| `Add` | Default; create an absent target and missing parent tables, preserving existing values |
| `Wrap` | Wait for a function, then wrap it and keep it available through `Frame::original` |
| `Replace` | Wait for a function, then replace it |

Use Add for a new function owned by the patch. Wrap is useful when the patch must do work before or after
an existing Lua function while retaining its behavior. Replace is for taking over an existing function
entirely. Choose deliberately: a name collision under Add is different from permission to replace another
mod's implementation.

Initialization and binding declarations apply to current and future VMs. Existing states receive work at a
supported Lua execution point after activation. Registration success accepts the declaration; errors
encountered while applying it in a particular VM are reported by the provider.

### When bindings become available

`Wrap` and `Replace` can be declared before the target function exists. If the target or a parent table is
nil, FC leaves the binding pending and retries during game updates or when the game registers a function
under that global name. A parent that exists but is not a table, or a target that exists but is not a
function, produces a conflict instead. `Add` reports a conflict if the target already contains any value.

For example, a wrapper for `{"ExampleMenu", "Enter"}` can wait for a script to create that table and function.
The wrapper applies at a subsequent supported execution point, so declaring it does not guarantee interception
of a call made immediately after the script defines `Enter`.

Once installed, a binding is not reapplied on every update. If a script later assigns another function to
the same key, that assignment replaces FC's binding. The provider does handle the game's C++ function
registration separately: it checks bindings under that global again and refreshes a wrapper's original
function. Arrange script loading and assignments accordingly.

Each exact binding path belongs to one FC declaration across all patches. A second registration for that
path returns an error during Prepare, even with `Wrap` or `Replace`; these modes do not chain multiple FC bindings.
This is separate from the per-VM checks against existing Lua values described above.

## Frame and value reference

Frames and their values borrow the current Lua invocation. Keep them on that Lua thread and discard them
when the callback returns. A copied Value still borrows the same invocation.

| `Frame` method | Meaning |
| --- | --- |
| `state()` | StateId for this VM lifetime; zero is absent and IDs are never reused |
| `argument_count()` | Number of incoming arguments |
| `argument(index)` | Zero-based argument; out-of-range reads return nil |
| `global(name)` / `global(name, value)` | Read or assign one literal global name |
| `table()` | Create a table |
| `value(bool/number/string)` | Create a value in this frame; strings are copied into Lua |
| `nil()` | Nil value without allocating another stack slot |
| `pointer(void *)` | Light userdata; pointed storage remains the author's responsibility |
| `userdata(tag, Bytes)` | Copy bytes into FC userdata with an exact type tag |
| `call(Value, arguments)` | Call a function in this frame; return Result<Results> |
| `call(FunctionRef, arguments)` | Call a retained live function belonging to this VM |
| `execute(source)` | Execute source in this VM and return Result<Results> |
| `retain(function)` | Create a persistent FunctionRef through this VM's registry |
| `original()` | In a Wrap callback, call the saved function with all incoming arguments |
| `original(span)` | Call it with exactly that span; an empty span means no arguments |
| `return_values(span_or_list)` | Select this callback's results; the latest call replaces earlier selections |
| `result()` | Read the first pending frame error without clearing it |
| `error(message)` | Record the first frame error; a binding raises it after C++ returns normally |

`call` accepts a span or initializer list of Values from the same frame. The default binding result is zero
values. `Results` owns a vector of borrowed Value views and provides `size()`, `operator[]` and `values()`.
Check its size before indexing; values still expire with the invocation.

`retain` returns `Result<FunctionRef>` and requires a function from this frame. Calling a FunctionRef requires
a live reference in the same VM; calling a Value requires the same frame. `original` is available only in a
Wrap callback. These failures return Error. A pending frame error prevents protected calls and takes
precedence over previously selected return values.

| `Value` method | Meaning |
| --- | --- |
| `type()` | Actual Lua Type, without coercion |
| `as_bool()` | Result<bool>; requires a boolean |
| `as_number()` | Result<double>; requires a number |
| `as_string()` | Result<string_view>; counted bytes may include NUL |
| `as_pointer()` | Result<void *>; accepts light userdata only |
| `as_userdata(tag)` | Result<Bytes>; accepts FC userdata with that exact tag |
| `has_metatable()` | Whether the value has a metatable |
| `get(key)` | Table access including metamethods; missing entries are nil |
| `set(key, value)` | Table assignment including metamethods; nil removes a field |
| `for_each(visitor)` | Visit stored entries; Result<bool> indicates completed traversal versus visitor stop |
| `result()` | This value's frame error |

Table keys can be Values, strings, booleans or numbers. A `for_each` visitor receives `(Value key, Value value)`;
return void to continue or false to stop. Order is unspecified, and entry views expire after each visitor
call. Traversal reports cleanup or Lua errors through its Result.

Lua tables are collections of keys and values. Assigning a Value into a table keeps the Lua value reachable
through that table; keeping the C++ Value wrapper after the callback would not be valid. A metatable can
customize table operations through Lua functions called metamethods, so `get` and `set` may execute Lua code
and fail. Use their documented error path rather than assuming every access is a direct memory lookup.

Userdata stores bytes supplied by C++ code in a Lua value. `userdata(tag, bytes)` copies a block that the helper
can later recognize by its tag. `pointer` instead exposes a light userdata pointer and leaves its storage
and lifetime with the author. Use a copied value or a retained reference where appropriate; a pointer alone
does not keep a game object alive.

`Type` values are Nil, Boolean, Number, String, Table, Function, LightUserdata, Userdata and Thread.
The game's Lua number representation is float32. The SDK uses double for access but cannot restore precision
already lost in Lua; creating a number converts to that float32 representation. Use strings or tagged
userdata for identities requiring more precision.

## Errors and wrapping

Lua operations can fail because a value has the wrong type, a script raises an error or a table operation
invokes failing Lua code. The helper protects the Lua call and translates these outcomes into Results or a
pending frame error so the C++ callback can finish normally. `frame.error` lets the binding report an expected
problem to its Lua caller, such as invalid arguments.

Conversions and protected calls return Result so your callback can handle expected failures. Table access
and value creation can instead leave a pending frame error. Check `frame.result()` when subsequent work
depends on them. A handled Result error is separate from the pending frame error.

```cpp
bindings.bind("ExampleFunction", fc::lua::BindingMode::Wrap, [](fc::lua::Frame &frame) {
    auto results = frame.original();
    if (!results) {
        frame.error(results.error().message);
        return;
    }
    frame.return_values(results->values());
});
```

This forwards all arguments and results of an existing function. Lua failures are handled under the
provider's protection and reported after C++ returns. Unexpected C++ exceptions escaping a callback are
fatal. Do not use Lua long jumps across C++ code or retain a raw Lua stack pointer.

## Retained functions and deferred work

Suppose a script gives C++ a function to call when an asynchronous operation completes. A Value from the
registration call will have expired by then. Retaining it creates a reference in the VM's Lua registry,
which keeps the function reachable while the VM exists.

`FunctionRef` owns one Lua registry reference. It is move-only. `state()` returns its VM, `operator bool`
checks current validity, and `reset()` invalidates the handle and schedules release on the owning Lua thread.
Destruction also resets it. A successful validity check does not guarantee a later call still finds the VM alive.

Use `Frame::retain` to keep a function beyond an invocation. Later, call it through a Frame for the same VM.
For work originating elsewhere, `Bindings::defer` transfers a callable to that VM's update processing.

### Keep a Lua callback for later

This handler exposes `Example.CallLater(callback)`. It retains the supplied function, queues it for later
Lua update processing and passes `"Ready"` when it runs. In the complete plugin above, replace `Handler`
with this class and use `fc::variant<DeferredCallbackHandler>`, keeping the dependency and supported profiles.

```cpp
#include <fc/lua.hpp>
#include <expected>
#include <utility>

class DeferredCallbackHandler {
  public:
    // Exposes a function that schedules a Lua callback after its current invocation ends.
    fc::Result<> prepare(fc::PrepareContext &context) {
        auto bindings = fc::lua::get(context);
        if (!bindings) {
            return std::unexpected(bindings.error());
        }

        return bindings->bind({"Example", "CallLater"}, [bindings = *bindings](fc::lua::Frame &frame) mutable {
            auto callback = frame.retain(frame.argument(0));
            if (!callback) {
                frame.error(callback.error().message);
                return;
            }

            // Save the owning VM before moving the reference into the queued callback.
            const auto state = callback->state();
            const auto dispatch = bindings.defer(state, [callback = std::move(*callback)](fc::lua::Frame &later) {
                auto result = later.call(callback, {later.value("Ready")});
                if (!result) {
                    later.error(result.error().message);
                }
            });
            if (dispatch != fc::lua::DispatchResult::Accepted) {
                frame.error(dispatch == fc::lua::DispatchResult::Full ? "Lua work queue is full" : "Lua state is unavailable");
                return;
            }

            frame.return_values({frame.value(true)});
        });
    }
};
```

Call it from a script after the binding is available:

```lua
local queued = Example.CallLater(function(message)
    print(message)
end)
print("Callback queued:", queued)
```

`CallLater` returns true once the callback is queued. The reference keeps the Lua function alive after the
registration frame ends. The queued C++ capture owns that reference, and FC supplies a new Frame for its VM
when executing the work. All arguments passed to `later.call` come from that new frame.

| `DispatchResult` | Meaning |
| --- | --- |
| `Accepted` | The provider owns the queued callback |
| `Unavailable` | No usable provider, live VM or active consumer |
| `Full` | The provider cannot reserve another queued job |

Rejected work releases its capture immediately; in this example that also releases the function reference
and raises an error in the calling Lua script. Accepted work runs once or is canceled if its VM or consumer
ends. Closing the VM before delivery cancels the call; the reference does not keep the VM alive.
FC destroys an accepted capture on the control thread after
completion or cancellation, and the reference's destructor schedules its Lua registry entry for release.

An error raised by the retained Lua function is reported through `later.error` and logged by the provider.
It cannot be returned to the original `CallLater` invocation, which has already finished.

For an asynchronous operation, keep the FunctionRef with that operation and submit the deferred call when
its result is ready. Copy result data into the queued capture; do not retain a Frame or Value from the
registration call. A multiplayer callback on FC's control thread can use this pattern to notify Lua.
`run` instead executes synchronously: call it only from a point already on that VM's Lua thread where Lua
work is appropriate.
