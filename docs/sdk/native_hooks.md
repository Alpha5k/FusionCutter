# Game functions and hooks

A patch can call a function already present in the game's executable or a DLL. A **hook** intercepts
execution at a chosen location and runs your callback. Hooks are useful when behavior must change each time
the game performs an action, rather than through a single fixed memory edit.

An **observer** receives information about a call without replacing its arguments or result. For example,
a diagnostic patch can record the inputs and outcome of a game function using observers; a behavior patch
can use a hook to change an input or result. If all that is needed is a stored constant, use a
[planned memory write](native_memory.md) instead.

Patches can [share a hook](#shared-hooks) at the same game location. FC combines compatible observers and
one replacement callback into a single installed hook, including when they come from separate plugins.

Include `<fc/native.hpp>`. Describe the game function's signature, reserve the function or hook during Plan, and use
its prepared handle from the correct game context. Calls through these handles run synchronously on the calling thread.

## Choose the scope of the interception

| Need | API | Execution affected |
| --- | --- | --- |
| Invoke an existing function from your code | `function` | Only calls you make through the handle |
| Replace a function's behavior | `hook` | Calls entering the hooked function entry |
| Change what one caller does | `hook_call` | Calls through that particular CALL instruction |
| Inspect arguments or results | `observe`, `observe_call` | The selected function entry or call site |
| Inspect or change registers inside a routine | `observe_instruction`, `hook_instruction` | Execution reaching that instruction location |

Suppose several game systems use the same function but only one caller needs a fix. Hooking that call site
keeps the change local to that caller. Hooking the function entry affects the other callers as well. Choose
the scope from the intended behavior and verify that execution really passes through the selected location.

## Calling existing code

For a compiler-supported convention, use a function-pointer type:

```cpp
using Compute = int (__cdecl *)(int);
fc::NativeFunction<Compute> compute_;

void plan(fc::Plan &plan) {
    compute_ = plan.function(compute_site_);
}
```

Here `compute_site_` is a verified `fc::FunctionLocation<Compute>`. The other forms are
`function<Call>(location, evidence)` and `function<Call>(rva, evidence)`.

The type says that the function accepts one integer and returns one integer. The location supplies its
address and evidence. Plan combines those descriptions into a `NativeFunction` handle; it does not call the
function. Later, `compute_.call(input)` invokes it on the current thread with the argument you provide.

`NativeFunction<Call>::call(arguments...)` returns `Result<ReturnType>`, including `Result<>` for void.
It works after preparation. An error means the runtime resource could not be used; a fault inside the called
function follows Windows exception handling. The author supplies valid arguments, object lifetimes and game timing.

Compiler pointer types support the normal x64 convention and x86 cdecl, stdcall, fastcall and thiscall.
A direct compiler-pointer call can use a variadic signature. Generated hook bridges require a fixed argument
list and an explicit `NativeCall` layout for record arguments or results. Direct compiler-pointer calls
leave record passing to the compiler.

A default-constructed NativeFunction has no target; `call` returns an error. Copying a prepared handle
refers to the same target or original function. It does not copy game state or change the calling thread.

## Explicit call layouts

A **calling convention** specifies where a function receives arguments, where it places its result and how
the caller and callee manage the stack. An ordinary C++ function-pointer type lets the compiler supply a
standard convention. Some game routines instead use register arrangements that require an explicit layout.

`NativeCall<Signature, Description...>` separates logical C++ values from the registers and stack locations
used by the game. For example, this x86 function receives its first value in ESI and the second on the stack:

```cpp
using NativeLookup = fc::NativeCall<
    std::uint32_t(std::uint32_t, std::uint32_t),
    fc::abi::X86<fc::abi::Args<fc::abi::Esi, fc::abi::Stack<0>>,
                fc::abi::Result<fc::abi::Eax>, fc::abi::CallerCleanup>>;
```

In this declaration, both arguments are logical `uint32_t` values. The X86 description puts the first in ESI,
the second in the first stack argument slot and the result in EAX. The caller removes the stack argument
after return. FC uses this information to build the code that transfers values between your C++ code and
the game routine. A correct address with an incorrect convention can still corrupt registers or the stack.

| Description | Meaning |
| --- | --- |
| `abi::Args<Storage...>` | One location for each logical argument, in signature order |
| `abi::Result<Register>` | Direct result storage |
| `abi::HiddenResult<Storage>` | Location of a caller-supplied pointer to a record result |
| `abi::Stack<Offset>` | Byte offset after the return address and, on x64, the 32-byte home area |
| `abi::CallerCleanup` | Caller removes x86 stack argument bytes |
| `abi::CalleeCleanup` | Callee removes x86 stack argument bytes |
| `abi::NoCleanup` | No x86 argument cleanup; also the x64 convention |
| `abi::X86<Args, Return, Cleanup>` | Full x86 layout; defaults to EAX result and caller cleanup |
| `abi::X64<Args, Return>` | Full x64 layout; defaults to RAX result |

Register tags are `Eax`, `Ebx`, `Ecx`, `Edx`, `Esi`, `Edi`, `Ebp`, `St0`; `Rax`, `Rbx`, `Rcx`, `Rdx`, `Rsi`,
`Rdi`, `Rbp`, `R8` through `R15`; and `Xmm0` through `Xmm15`. Availability and valid value widths depend on
the architecture and operation. Stack and instruction pointers are not argument register tags.

The compact form lists storage tags for leading arguments and a cleanup tag, for example
`NativeCall<int(int, int), abi::Esi, abi::CallerCleanup>`. Remaining arguments use inferred stack slots from
offset zero; the build's architecture and normal result register apply. Use the full form when every
physical location needs to be explicit, especially for hidden results or mixed register/stack layouts.

Arguments and results must be trivially copyable values rather than C++ references. Explicit record layouts
must reflect how the game function actually passes the record. The runtime validates the requested layout;
it cannot infer a game's calling convention from the C++ type.

## Replacing a function or call site

`hook<Call>(location, callback, evidence)` replaces a function entry.
`hook_call<Call>(location, callback, evidence)` replaces the callee at one direct CALL site. Both return an
original-call handle. Typed FunctionLocation and CallLocation overloads infer Call; bare-RVA overloads are
also available.

```cpp
#include <fc/native.hpp>

class ComputeHook {
  public:
    using Compute = int(__cdecl *)(int);

    // Registers a replacement using the caller's verified function location.
    void add_to(fc::Plan &plan, const fc::FunctionLocation<Compute> &site) {
        original_ = plan.hook(site, [this](int input) noexcept -> int {
            auto result = original_.call(input);
            if (!result) {
                fc::fatal(result.error());
            }
            return *result;
        });
    }

  private:
    fc::NativeFunction<Compute> original_;
};
```

This example forwards unchanged behavior. Losing an already prepared original is treated as fatal rather
than inventing a replacement result. A real replacement can change arguments, return a different value,
or deliberately omit the original call.

`add_to` runs during the enclosing handler's Plan method with a verified site. The lambda is stored for later;
it does not execute during Plan. When the game reaches the hook, `input` is the argument passed by the game, and
`original_.call(input)` reaches the original behavior through FC's prepared handle. Calling that handle
avoids reentering the replacement through the hooked entry.

The helper itself must remain alive because the lambda captures `this`; keeping it as a handler member
provides that lifetime. To observe the result without changing behavior, the observer interface below
expresses the purpose more directly than a replacement that only forwards the call.

The callback must be `noexcept` and return a compatible result. Captures are retained in the plugin, but
objects referenced by a capture must live for every installed use. The original handle is prepared before
the hook becomes reachable.

Typed callbacks may take `const fc::NativeContext &` before the game function's arguments. Its `return_address()`
reports the original caller's return address, or zero when unavailable. This is call context, not a Plan RVA.

The context belongs to that invocation. Copy the numeric return address if it is needed later. Instruction
callbacks use CpuContext instead and do not supply a caller return address through NativeContext.

| Plan operation | Return |
| --- | --- |
| `function`, `hook`, `hook_call` | `NativeFunction<Call>`, usable after resource preparation |
| `observe`, `observe_call` | `void`; callback registration is part of the plan |
| `hook_instruction`, `observe_instruction` | `void`; callbacks receive the saved CPU context |
| `expect_fault` | `ExpectedFault`, usable after resource preparation |

All are planning operations. Invalid sites, evidence mismatches, unsupported call layouts and conflicts
fail the shared plan. The returned handle is a reservation, not a separate indication that installation
succeeded. [Typed redirects](native_memory.md#addresses-operands-and-branches) also return an original
function handle; redirects taking an Address return void.

## Observing calls

Use `observe` for a function entry and `observe_call` for one direct CALL site. Wrap callbacks in `before`
and/or `after`. Observers leave the arguments and result unchanged.

`before(callback)` returns `Before<CallbackType>` and `after(callback)` returns `After<CallbackType>`.
Each owns its callable; Plan retains the submitted callbacks for installed use:

```cpp
#include <fc/native.hpp>

using Compute = int(__cdecl *)(int);

struct Observation {
    int input = 0;
};

// Retains per-call input for the matching after callback, including recursive calls.
void observe_compute(fc::Plan &plan, const fc::FunctionLocation<Compute> &site, fc::Logger log) {
    plan.observe<Observation>(site, fc::before([](const int &input, Observation &state) noexcept {
                                  state.input = input;
                              }),
                              fc::after([log](const int &, const int &result, const Observation &state) noexcept {
                                  log.debug("Compute({}) returned {}", state.input, result);
                              }));
}
```

Callback argument order is:

| Callback | Arguments after optional NativeContext |
| --- | --- |
| Before | Const function arguments, then optional mutable State |
| After, non-void function | Const function arguments, const result, then optional const State |
| After, void function | Const function arguments, then optional const State |

Without state, omit the last argument. With a Location or RVA, template arguments are `<Call, State>`.
With a typed function/call location, only `<State>` is needed. State must be trivially copyable and
`noexcept` default-constructible; the runtime creates separate storage for each invocation and releases it
without invoking a destructor.

The example saves the input before the function and pairs it with the result afterward. Per-call State is
useful when the function recurses or multiple threads call it at once: one handler member would otherwise
mix information from different invocations. Leave State out when each callback can do its job using only
the arguments and result it receives.

## Shared hooks

A **hook site** is the game address where FC intercepts execution. Several patches may need that address:
one might fix a function's behavior while others collect diagnostics or respond when it runs. Shared hooks
let those patches coexist. The runtime installs one hook and calls each patch's registered callbacks.

Sharing is automatic. Each patch uses `hook` or `observe` in its own plan, describing the same function
location and calling convention. There is no sharing flag or dependency to add merely to share the site.
The same facility applies to `hook_call` with `observe_call`, and `hook_instruction` with
`observe_instruction`.

For example, one patch can keep the `ComputeHook` helper above as a handler member and call its `add_to`
method during Plan. Another patch can call `observe_compute` with its own plan and logger. If both use the
same verified function location and compatible call descriptions, the observer runs around the replacement.
The diagnostic patch can also run on its own, observing the original function when the replacement is absent.

### What can share a site

A **modifier** is the replacement callback registered by `hook`, `hook_call` or `hook_instruction`.

| Combination at one site | Outcome |
| --- | --- |
| Multiple compatible observers | All run; FC preserves the original function call or instruction sequence |
| One modifier and compatible observers | All observers run around the modifier |
| Two modifiers | Conflict, even if both intend to call the original |
| Different hook kinds or incompatible call descriptions | Conflict |
| An overlapping instruction write or a hook starting inside another hook's instruction range | Conflict |

Shared declarations must identify the same address and cover the same instruction range. Function and call
hooks must also agree on argument and result types, register and stack locations, and stack cleanup.
Using the same Call type and verified location is the simplest way to keep those declarations consistent.
Two separate CALL instructions targeting the same function are separate hook sites.

Each patch still supplies its own evidence. When another FC hook is already installed, the runtime checks
that detour and uses the saved original bytes for evidence checks. Authors describe the original game code;
they do not need to match bytes written by another FC patch. This sharing applies to hooks managed by FC.

A conflict fails the new plan against an already installed or earlier accepted plan. The runtime reports
the conflicting patches and address range, and the rejected patch's
[failure policy](lifecycle.md#errors-and-failure-policy) determines whether startup may continue.

### Callback order and results

Before callbacks run in ascending order of the full `PluginId.PatchId`, using ASCII case-insensitive
comparison. Multiple registrations from one patch keep their declaration order. After callbacks run in
reverse order. For two observers, `Example.Alpha` and `Example.Beta`, the sequence is:

```text
Example.Alpha: before
Example.Beta: before
Modifier, or original function if there is no modifier
Example.Beta: after
Example.Alpha: after
```

Dependencies control patch installation, not this callback order. Each callback runs synchronously on the
thread that reaches the site. Paired observer State belongs to that observer and that invocation, so other
observers, recursive calls and concurrent calls have separate state.

Typed before and after callbacks receive the arguments captured when the game reached the hook. After
callbacks also receive the final return value, including a modifier's replacement result. If a modifier
calls the original with different arguments, those arguments do not replace the observers' captured arguments.

The original-call handle returned by `hook` or `hook_call` bypasses that site's callbacks. Calling it from
the modifier does not run the same observers a second time. Calling it elsewhere also bypasses that site;
other hooked locations reached by the original code still run normally. When a modifier is present, it
decides whether to call the original at all; after observers still see the result it returns.

Instruction hooks use the same observer order around an optional modifier. Before observers see the
incoming CPU context and after observers see the modifier's edits. All of these callbacks run before the
game resumes the instruction sequence, as described below.

## Instruction hooks

An instruction hook is useful when the required location lies inside a routine and has no useful function
signature. Its input is a saved CPU register context. The author must know which registers and flags have
meaning at that exact point and how changing them affects the remaining instructions. Prefer typed calls
and hooks when a verified function signature already expresses the required behavior.

`hook_instruction(location, callback, evidence)` passes mutable `CpuContext &`. Changes to saved registers
determine resumed execution. `observe_instruction` passes `const CpuContext &`, using `before` and `after`
around the modifier. These callbacks run before the game instruction sequence resumes; an instruction
after-observer is not a callback after the displaced game instructions execute.

All forms accept Location or RVA plus evidence, or CodeLocation containing evidence. Paired instruction
observers can use `<State>`; before receives mutable State and after receives const State.

`CpuContext` aliases the C record for the current architecture:

- x86 exposes `eflags`, `edi`, `esi`, `ebp`, `esp`, `ebx`, `edx`, `ecx`, `eax`, `eip` and XMM storage.
- x64 exposes `rflags`, `r15` through `r8`, `rdi`, `rsi`, `rbp`, `rsp`, `rbx`, `rdx`, `rcx`, `rax`, `rip` and XMM storage.

Use `stack_pointer(context)` and `frame_pointer(context)` when the code only needs those addresses.
The exact register records are in [native.h](../../sdk/include/fc/abi/native.h). Keep architecture-specific
register handling with the relevant game layout, and select game behavior by profile.

## Expected access violations

`expect_fault(location, kind, evidence)` reserves recovery for one exact instruction and access-violation
kind. Location, CodeLocation and RVA forms return `ExpectedFault`, usable after preparation.

`NativeException` has `ReadAccessViolation`, `WriteAccessViolation` and `ExecuteAccessViolation`.
`ExpectedFault::invoke(noexcept_callback)` runs synchronously and returns:

| `FaultOutcome` | Meaning |
| --- | --- |
| `Unavailable` | The handle or runtime facility cannot be used |
| `Completed` | The callback returned normally |
| `Caught` | The declared instruction raised the declared access violation and recovery occurred |

Use this only for a known game instruction whose expected fault is part of its behavior. Other instructions
and exceptions do not match. Recovery can skip local destructors inside the callback, so keep required
cleanup outside that invocation. It is not a general exception wrapper for patch code.
