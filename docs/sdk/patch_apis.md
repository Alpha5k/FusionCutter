# Sharing APIs between patches

A patch API lets one installed patch call functionality provided by another in the same game process.
The patch implementing the functionality is the **provider**; a patch using it is a **consumer**. The
**contract** describes the callable functions and data both sides agree to exchange.

This is useful when several patches need one integration or shared state. For example, Core's Lua bindings
patch owns the game's Lua integration, while consumers register their own Lua functions through its API.
Each consumer can focus on its behavior without installing another copy of the same game hooks.

Use ordinary functions or classes for private implementation details that need no independent patch lifecycle.
Use a patch API when separately selected patches or external plugins must cooperate. These calls stay inside
one process; [multiplayer messaging](multiplayer.md) is the separate facility for communication with peers.

Include `<fc/patch_api.hpp>` or `<fc/plugin.hpp>`. A provider publishes a table during Prepare. A consumer
retrieves the compatible table from a declared dependency, or subscribes to an optional provider.

The table uses ordinary, typed C++ calls while preserving a C-compatible representation between DLLs.
Share the contract header with consumers so they can use the API without searching the provider source.

## A provider and required consumer

The following complete plugin has two patches. Counter owns a number and increases it during updates.
Display requires Counter and shows that number in its status. The consumer knows the shared `CounterApi`
contract, but it does not access Counter's fields or construct a Counter object.

```cpp
#include <fc/plugin.hpp>

namespace example {

struct CounterApi {
    fc::Function<std::uint32_t() noexcept> value;
};

// Describes every table entry in declaration order.
auto counter_contract() {
    return fc::api<CounterApi>("Example.Counter", &CounterApi::value);
}

class Counter {
  public:
    // Exposes this handler's counter through a copied function table.
    fc::Result<> prepare(fc::PrepareContext &context) {
        return context.publish(counter_contract(), CounterApi{fc::bind<&Counter::value>(*this)});
    }

    // Advances the counter once per runtime update.
    void update(const fc::UpdateContext &) noexcept {
        ++value_;
    }

    std::uint32_t value() const noexcept {
        return value_;
    }

  private:
    std::uint32_t value_ = 0;
};

class Display {
  public:
    // Retrieves the installed dependency's counter API.
    fc::Result<> prepare(fc::PrepareContext &context) {
        auto api = context.get("Counter", counter_contract());
        if (!api) {
            return std::unexpected(api.error());
        }
        counter_ = *api;
        return {};
    }

    // Displays the provider's current counter in this patch's status.
    void write_status(fc::StatusContext &status) const noexcept {
        status.line("Counter: {}", counter_.value());
    }

  private:
    CounterApi counter_{};
};

// Declares the provider and its dependent status patch.
fc::Plugin build_plugin() {
    return {
        .id = "Example",
        .version = "1",
        .patches = {fc::Patch{.id = "Counter", .enabled = true, .variants = {fc::variant<Counter>({"BF2.*"})}},
                    fc::Patch{.id = "Display", .enabled = true, .depends_on = {"Counter"}, .variants = {fc::variant<Display>({"BF2.*"})}}}};
}

} // namespace example

FC_PLUGIN(example::build_plugin)
```

`publish(contract, table)` copies the table and contract. Bound objects must outlive installed uses.
`get(provider, contract)` returns a table copy from an installed declared dependency. A failed required
connection rejects the consumer; ignoring its returned error does not make the dependency optional.

Both example handlers use only the control thread. A provider called from hook or Lua callbacks must
also account for those threads when reading or changing shared state.

`CounterApi` is a table with one callable member, `value`. `fc::bind` connects that member to the Counter
handler's method and object. `counter_contract` describes the table under a stable API ID so FC can check
that the provider and consumer agree before connecting them.

The dependency makes Counter install first. Display's Prepare retrieves its API and retains the table copy.
Later, `counter_.value()` calls the provider's method synchronously and returns the current number. The table
copy contains a callable referring to the provider; it is not a copy of the provider's counter state.

The provider patch reference and the contract ID have different purposes. `"Counter"` chooses the local
patch; `"Example.Counter"` in `counter_contract` identifies the API's meaning and layout. They happen to
use similar names in this example but need not match. Share the table and contract declaration in a header
when provider and consumer are built in different DLLs.

## Optional providers

Some integration is an enhancement rather than a requirement. A patch might work on its own and add a
status display through another plugin when that plugin is installed. Making that provider a dependency
would prevent the entire patch from running when it is absent. `on_available` lets the patch start and
connect optional behavior later instead.

During Prepare, register a `noexcept` callback accepting `const Table &`:

```cpp
return context.on_available("Example.Counter", example::counter_contract(),
    [this](const example::CounterApi &api) noexcept {
        counter_ = api;
    });
```

No dependency is required. Delivery happens once on the control thread after both patches activate, including
when the provider installs later. An absent provider produces no callback. A selected provider with an
incompatible contract produces a warning instead of invoking the callback. Keep optional behavior inactive
until a table arrives, and check any `Function` before calling it.

## Prepare method reference

| Method | Return | Expected failure |
| --- | --- | --- |
| `publish(contract, table)` | `Result<>` | Invalid contract, duplicate contract ID in this patch, or a call outside Prepare |
| `get(provider, contract)` | `Result<Table>` containing a copy | Provider is not an installed declared dependency, API is missing, or contract/functions are incompatible |
| `on_available(provider, contract, callback)` | `Result<>` accepting the registration | Invalid provider reference, contract or registration stage; an absent provider is allowed |
| `callback(function)` | `ConsumerCallback<Signature>` | Construction does not invoke the callback; `try_call` checks whether it can run |
| `callback<&Type::method>(object)` | The same callback type, inferred from the method | The object must survive installed uses |

The three methods returning Result also fail if the runtime lacks the patch API service. Runtime rejections
of these Prepare operations record a patch failure even if the caller ignores the Result. A later optional
provider mismatch is different: it warns and leaves that optional connection unavailable.

Provider references name a patch, using `Patch` or `Plugin.Patch`, and compare ASCII case-insensitively.
A dependency on a group permits retrieval from its members, but `get` still names the individual provider
patch. API contract IDs compare exactly, including case. A provider may publish several different API IDs.

The optional callback receives a borrowed `const Table &`. Copy the table when retaining it, as the example
does. The copied Function entries continue to refer to the provider's objects.

## Function bindings and consumer callbacks

The direction of a call determines which wrapper is useful. A published `Function` lets a consumer invoke
the installed provider. A retained `ConsumerCallback` lets the provider call back into a consumer whose
startup may not yet have completed. The runtime checks that consumer's installed state before the call.

`Function<R(Args...) noexcept>` stores a `context` pointer and C function pointer. `operator bool` checks
whether it has a function. `operator()` invokes it synchronously; it does not switch threads.

An empty Function converts to false and must not be called. It does not return a failed Result automatically.
The contract chooses each function's return type and how that function reports its own errors.

- `bind<&Type::method>(object)` binds a const or non-const `noexcept` member.
- `bind<&free_function>()` binds a `noexcept` free function.
- The bound object remains owned by its original DLL.

When a provider retains a callback into another patch, use `ConsumerCallback<Signature>` so the runtime can
check that consumer's installed state. Construct it during Prepare with `context.callback(function)` or
`context.callback<&Type::method>(object)`.

```cpp
using Changed = fc::ConsumerCallback<void(std::uint32_t) noexcept>;

// In a consumer's prepare method, pass this to a provider's registration function.
auto changed = context.callback<&Handler::changed>(*this);

// In the provider, invoke its retained Changed callback on the contract's documented thread.
auto delivered = changed.try_call(42);
```

`try_call` returns `std::expected<R, CallbackError>`. The only `CallbackError` is `Inactive`: the consumer
has not activated, failed startup, or lacks a usable binding/service. Accepted calls preserve the caller's
Win32 and Winsock error values and attribute callback failures to the consumer. They still run on the caller's
thread. Bound objects must remain alive; a callback is not an owning reference to an arbitrary C++ object.

## Contract types and records

The contract is needed because plugins can be compiled separately and distributed by different authors.
Matching a name alone would not show whether both sides agree on the table's fields or parameter layouts.
FC checks the described layout before exposing the callable table. Authors must also keep its behavioral
meaning compatible, including ownership and permitted threads.

API tables contain `Function` members. `api<Table>(id, &Table::member...)` lists every function once in
declaration order and builds an `ApiContract<Table>`. The runtime checks the identity, layout description,
table size/alignment and non-null required function pointers. It does not adapt incompatible table versions.

Arguments and results use fixed-width integers, enums with a defined underlying type, float/double, pointers,
fixed arrays, `TextView`, `ByteView`, described records, `Function` or `ConsumerCallback`. Records must be
standard-layout and trivially copyable. Use integer 0/1 instead of bool. References and owning STL objects
do not cross this boundary.

Describe nested records in their own namespace using argument-dependent lookup:

```cpp
namespace example {

struct Counts {
    std::uint32_t accepted;
    std::uint32_t rejected;
};

auto fc_description(std::type_identity<Counts>) {
    return fc::describe<Counts>("Example.Counts", &Counts::accepted, &Counts::rejected);
}

} // namespace example
```

`RecordDescription` owns the encoded layout. List every field once in declaration order. Pointer cycles
refer to the active record identity. `ApiContract::view()` exposes a temporary C view of its owned bytes
for adapters; normal authors pass the contract directly to PrepareContext.

`describe<T>(id, members...)` returns RecordDescription; `api<Table>(id, members...)` returns ApiContract.
Their IDs must be nonempty. Types with described fields must also permit value initialization so the SDK
can measure member offsets. The SDK cannot discover omitted fields: both sides must use the same complete
contract header. Build descriptions during declarations or Prepare, rather than repeatedly in live calls.

`TextView` is `FC_Text` and `ByteView` is `FC_Bytes`: pointer plus byte count, with no required terminator.
Document who owns their storage and how long returned or retained views remain valid.

Use a new API identity when behavior becomes incompatible, even if the binary layout stays the same.
Document the permitted calling thread, ownership, errors and callback behavior in the shared contract header.
An SDK helper can wrap a core provider, as [`fc::lua::get`](lua.md) does, without adding another runtime API.
