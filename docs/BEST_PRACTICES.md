# Plugin Best Practices

This document explains how to build FusionCutter plugins around the framework instead of beside it. It is practical
guidance for both human authors and coding tools.

Read [GETTING_STARTED.md](GETTING_STARTED.md) for a first plugin and [SDK_REFERENCE.md](SDK_REFERENCE.md) for the full
public API.

## Build around patches

A plugin is a distributable catalog of patches. A patch should represent one user-visible behavior that can be selected,
configured, diagnosed, and attributed independently.

- Keep unrelated behavior in separate patches, even when it ships in the same DLL.
- Keep behavior that must succeed or fail as one unit in one patch.
- Use categories and groups for presentation and shared selection, not as hidden execution mechanisms.
- Give IDs stable semantic meaning. Configuration and cross-patch relationships outlive display names.
- Declare only target combinations that the implementation and target facts actually support.

Avoid a single manager patch that privately selects, configures, installs, or reports several unrelated features. That
hides work the framework is designed to own.

## Let the framework own installation

Describe native work through `Plan`. Do not perform the same job through direct memory writes, calls that change memory
protection, an independent hook library, or a private patch scheduler.

Use the narrowest operation that states the intent:

| Need | SDK facility |
| --- | --- |
| Read native data | `Plan::require` |
| Mutate native data during runtime | `Plan::require_mutable` |
| Replace bytes or a typed value | `Plan::write` |
| Remove instructions | `Plan::nop` |
| Redirect a call or jump | `Plan::redirect_call` or `Plan::redirect_jump` |
| Own a function or instruction hook | `Plan::hook` |
| Observe an existing or future hook | `Plan::observe` |
| Reserve native storage | `Plan::allocate_data` |
| Consume an optional service | `Plan::bind` |

This gives the framework one view of requirements, evidence, conflicts, ownership, failure attribution, and commit. Raw
native access is appropriate only after the SDK explicitly returns a prepared pointer, handle, original call, interface,
or callback context for that purpose.

## Keep lifecycle work in its phase

Each lifecycle phase has a deliberately narrow job.

| Phase | Responsibility |
| --- | --- |
| Construction | Initialize lightweight state from copied settings and target facts. |
| Plan phase | Declare the complete native transaction without changing the process. |
| Prepare phase | Resolve symbolic storage, acquire substantial resources, and find required interfaces. |
| Commit phase | Framework-owned validation and native mutation; plugins do not implement it. |
| Activate phase | Arm prepared work with bounded operations that cannot fail. |
| Update phase | Perform bounded, nonblocking work on the serialized loader pump. |
| Status | Publish a small, side-effect-free snapshot of current state. |

Do not call game functions, mutate game memory, or acquire substantial resources during the Plan callback. Do not hide
fallible setup in the Activate phase. Avoid significant work in DLL initialization, global constructors, callbacks, or
the Update phase.

Omit lifecycle methods that have no work. An empty callback creates a false impression that a phase is significant.

## Treat target facts as owned evidence

An RVA, calling convention, structure layout, byte pattern, vtable slot, or instruction span is part of the patch's
implementation for a particular target profile.

- Declare locations with the correct typed location class and a meaningful name.
- Supply evidence whenever stable bytes or relationships can verify the fact.
- Keep distinct facts for distinct layouts and executable images.
- Select one executable image per patch. Split cross-image behavior into cooperating patches.
- Do not infer support from filename similarity or reuse an address from another edition without analysis.
- Keep proprietary binaries and extracted game content outside the repository.

Evidence is a guard against a wrong binary or stale fact, not a signature-scanning system. Prefer the smallest stable
evidence that proves the intended site without depending on unrelated compiler output.

## Model native calls exactly

Use an ordinary function-pointer type when the compiler can express the complete native call. Use `NativeCall` only for
calls that require explicit ABI metadata.

The declaration must account for the result, arguments, hidden results, register assignment, stack placement, cleanup,
and architecture. A type that happens to compile is not evidence that it models the native boundary correctly.

Keep native data and interface contracts plain and bounded. Do not pass C++ standard-library ownership, references,
exceptions, RTTI objects, or allocator-dependent types across DLL boundaries.

## Share hooks instead of stacking detours

One patch owns a native hook site. Other patches observe that site when they need notification without changing the
operation.

- Use `hook` only when the patch must replace or control behavior.
- Use `observe` when the patch only needs before or after notification.
- Use paired observations when an invocation needs small temporary state between before and after callbacks.
- Keep invocation state trivial, bounded, and local to that call.
- Preserve the original call's error state unless changing it is part of the patch's documented behavior.

Do not install several private detours on the same address or recreate an earlier callback pipeline. Shared hooks give
the framework deterministic ownership and composition.

## Choose relationships deliberately

Dependencies, includes, and interfaces solve different problems.

| Mechanism | Use it when |
| --- | --- |
| `depends_on` | This patch cannot be selected or activated successfully without another patch. |
| `includes` | Selecting this patch should also select another patch when that patch is available. |
| Required interface lookup | This patch needs a service from an active declared dependency during the Prepare phase. |
| Optional interface binding | This patch can use a service when a provider activates, but remains valid without it. |

Do not add a hard dependency merely to enforce a preferred order. Do not use an include where correctness requires the
other patch. Do not import or link against another plugin DLL; communicate through framework relationships and copied
interface contracts.

Give one patch clear ownership of each interface. Keep the contract small, versioned by ID, and independent of either
plugin's private representation. Use `InterfaceFunction` for explicit service calls and `Observation` for transparent
notifications that must preserve the caller's ambient error state.

## Use typed settings

Describe user configuration with an SDK settings schema and consume the typed settings object supplied to the handler or
Plan callback.

- Give every setting a useful description and a safe default.
- Constrain numeric values and string lengths to the range the implementation can handle.
- Use choices for finite semantic values instead of parsing arbitrary strings later.
- Use named sections and finite string maps only when they make the configuration easier to understand.
- Use a whole-schema validator for relationships that individual field constraints cannot express.
- Declare environment overrides through the schema rather than reading the environment independently.

Validation should be deterministic, bounded, and free of native side effects. A plugin should not maintain a second
configuration file or parser for behavior the framework can represent.

## Make ownership and failure visible

Prefer ordinary C++ ownership within the plugin and SDK-owned values at framework boundaries. Prepare resources before
exposing them, release them through the handler's lifetime, and avoid detached background work that can outlive the
plugin state it uses.

Use `FailurePolicy::Continue` for an independent patch whose failure can be attributed and isolated. Use
`FailurePolicy::Fatal` only when continuing would leave the framework or target process in an unsafe or fundamentally
invalid state. Fatal policy is not available for support whose executable image can arrive late. Relationships should
express real correctness requirements so dependent failures propagate naturally.

Return useful `Error` details from fallible author work. Do not catch and suppress a failure merely to keep a patch
listed as successful.

## Keep callbacks bounded

Assume native hook and observation callbacks are latency-sensitive and may run on arbitrary game threads.

Avoid:

- heap allocation and deallocation;
- file, console, or network I/O;
- ordinary logging;
- blocking locks or waits;
- unbounded loops or container growth; and
- work that depends on the loader pump running immediately.

Use atomics, fixed-capacity state, or another design with a clear time bound. Move deferred processing to `update`, but
keep the Update phase nonblocking and bounded as well. Use a trace channel when high-volume event capture is genuinely
needed.

Document the concurrency model where shared state, callback lifetime, or synchronization is not obvious from the type.

## Use the right diagnostic channel

Diagnostics should make selection, installation, and runtime health understandable without flooding the log.

| Information | Facility |
| --- | --- |
| Lifecycle decisions, resolved target facts, policies, and first-event confirmation | Debug log |
| A meaningful successful transition or user-relevant event | Info log |
| Degraded behavior that remains usable | Warning log |
| A failure that prevents intended behavior | Error log or returned `Error` |
| Current counters, modes, bindings, and health | Status |
| High-volume structured events | Trace channel |

Include enough context to identify the patch, operation, and relevant target or policy. Avoid logging every callback,
repeating stable status, narrating obvious control flow, or emitting both a returned error and redundant copies of the
same message.

Comments serve a different purpose: document what the code does, why it exists, and the context a reader needs to
maintain it. Write for readers who have only the code and public SDK documentation. Follow the project's comment rules
for both production and test code.

## Test the behavior that carries risk

Use `FusionCutter::Testing` scenarios to validate every supported layout and role against canonical private images.
Check selection, configuration, Plan generation, evidence, and conflicts that are meaningful to the patch.

Add focused unit or integration tests for logic that a scenario intentionally does not run, including the Prepare and
Activate phases, updates, live callbacks, and status. Test through the public boundary where practical.

Keep the suite to the minimum set that proves the intended behavior:

- do not duplicate the same assertion at several layers without a distinct risk;
- do not add tests for SDK or compiler behavior the plugin does not own;
- do not encode speculative future requirements; and
- do not weaken a production design merely to make it easier to test.

A proprietary fixture belongs in a private local corpus, not in the plugin repository.

## Package only what the plugin owns

Build with the installed SDK target and `fc_add_plugin`. Produce separate x86 and x64 artifacts. Keep any required
native dependency beside the plugin so the loader's documented dependency search can find it; statically link a
dependency when that is practical and permitted.

Do not depend on another plugin's private DLL exports, assume the user's working directory, or require a global PATH
change. Keep symbols and source metadata in release artifacts when they materially improve crash or failure diagnosis.

## Before release

Confirm that:

- each patch has one clear purpose, stable IDs, and accurate support declarations;
- all target facts and native call descriptions have been reviewed per target profile;
- the Plan contains every native mutation, hook, observation, and symbolic allocation;
- lifecycle work occurs in the correct phase and callbacks are bounded;
- settings, relationships, interfaces, diagnostics, and status use SDK facilities;
- failures are attributed and use the least severe correct policy;
- canonical image scenarios cover the supported matrix;
- runtime-only behavior has the minimum additional tests needed to establish confidence; and
- the shipped DLL and its dependencies match the target architecture and documented package layout.
