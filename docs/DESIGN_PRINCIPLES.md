# FusionCutter Design Principles

This document defines the project-wide principles that guide FusionCutter development. It applies to the framework,
loaders, SDK, tools, and tests. External plugins are separate projects whose authors choose their own internal design.

See [CONTRIBUTING.md](../CONTRIBUTING.md) for setup, build, and test instructions. See
[STYLE_GUIDE.md](STYLE_GUIDE.md) for source formatting, naming, organization, and comments.

## Project priorities

Aim to satisfy all of these priorities. When a genuine tradeoff remains, use the following order:

1. readability;
2. modularity;
3. safety;
4. performance; and
5. simplicity.

The order does not make later priorities optional. It determines which concern wins only after reasonable attempts to
satisfy all of them.

### Readability

The project should be understandable to someone who did not write it. Important behavior, ownership, assumptions, and
failure consequences should be visible where the work happens rather than scattered across hidden registration,
duplicated state, or unrelated files.

Use direct control flow, descriptive names, focused abstractions, and comments that preserve reasoning the code cannot
express. Keep related behavior together and make important lifecycle and failure boundaries visible. Ordinary changes
should remain local and predictable as the project grows.

Readable code is neither compressed nor padded unnecessarily. Repetition, excessive indirection, and needless
intermediate steps can obscure code just as easily as dense expressions or clever templates. Prefer the most concise
form that remains clear; when two implementations are equally understandable, the shorter one is generally better.

### Modularity

Each component should have a clear purpose, owner, and boundary. Changing one component should not require unrelated
changes elsewhere. Shared behavior should have one deliberate home rather than being copied or reached through private
implementation details.

New patches, plugins, targets, and framework capabilities should extend the existing model without creating parallel
paths or collections of special cases. Room to grow means choosing sound building blocks, keeping boundaries clean,
and allowing likely extensions. It does not mean building speculative systems before their value is understood.

### Safety

Safety applies throughout the framework's lifecycle, not only when it writes memory. Native operations should use the
framework's coordinated paths for validation, conflict detection, installation, rollback, and reporting. Ownership,
lifetime, concurrency, and failure behavior should be explicit wherever they affect correctness.

Safeguards should address credible failures and provide guarantees that contributors can understand and maintain.
Additional machinery is not an improvement when it duplicates existing protection, obscures the operation, or costs
more than the risk justifies. Fail locally when the resulting state is known; stop when continuing is no longer safe.

### Performance

Costs should match the work being performed. Complete work once when possible instead of repeating it in game hooks or
other timing-sensitive paths. Recurring runtime work must remain bounded and predictable, without blocking or
unbounded operations.

Optimize where latency, frequency, or scale makes the cost meaningful. Cold initialization code should favor clarity
unless its cost is significant. Performance improvements should not make ordinary code harder to understand without a
demonstrated need.

### Simplicity

Choose the simplest clear and complete solution that meets the requirements. Minimize unnecessary concepts, states,
representations, layers, special cases, and code.

Line count matters, but it is not the only measure of simplicity. Prefer less code when two forms are equally clear and
capable. Do not expand a concise, readable operation merely to make it look more explicit, and do not compress code
when doing so hides behavior or requires specialized knowledge to understand it.

Modern C++ and useful abstractions are encouraged. An abstraction may hide substantial complexity when it gives that
complexity one clear owner and presents callers with a smaller, understandable contract. An abstraction does not help
when it merely renames a straightforward operation, duplicates another model, or adds more machinery than it removes.

## Framework principles

### Consistent framework behavior

Equivalent operations should follow a common path. Built-in, bundled, and external plugins should use the same
admission, lifecycle, validation, failure, and reporting model. Patches waiting on a late image should reuse that model
and differ only where the image is unavailable during startup.

Extend the component that owns a capability rather than creating a parallel path. If a requirement forces unnecessary
complexity, propose a simpler revision instead of building around it.

Each fact should have one clear authority. Derived records and published snapshots are valid when their purpose and
relationship to that authority are explicit. When ordering affects behavior, define it rather than relying on
incidental filesystem, registration, or container order.

### Ownership and boundaries

The public SDK is the boundary between the framework and plugins. Framework code must not depend on a plugin's private
implementation, and the SDK must not expose private framework code.

The framework owns shared behavior and target facts. Patch-specific locations, settings, structures, and behavior
remain owned by the plugin that uses them. A missing framework capability should lead to the simplest general
extension, not a one-off exception in the framework.

Every resource and piece of retained state should have a clear owner and lifetime, including failure paths and native
boundaries. Public headers and interfaces must be self-contained, and third-party types should remain behind the
component that owns them.

### SDK usability

The SDK should keep common patches concise while supporting large, stateful features. Advanced capabilities should
compose with the ordinary model rather than add boilerplate to simple patches. Public operations should describe game
and framework concepts directly without requiring knowledge of framework internals or the template machinery used by
adapters.

### Lifecycle responsibilities

Each operation should run in the lifecycle stage that owns it. Registration and definitions are declarative and must
not start runtime work. Substantial patch resources are acquired only after dependency, validation, and conflict
checks succeed. Runtime callbacks use prepared state and remain bounded and nonblocking.

### Native integration

The framework and SDK should express native assumptions and target-specific facts clearly. Validation evidence remains
optional; omitting it skips only the evidence comparison, while every other framework check still applies.

Framework-coordinated native writes, hooks, and installation must use the common planning, validation, and installation
path. If that path cannot express a required operation, add the simplest general capability instead of a
framework-specific workaround.

### Failure handling

Isolate patch failures while the framework still has a well-defined process state. Required consumers should fail,
controlled changes should be rolled back where possible, and exposed native state should remain owned when releasing
it would be unsafe. Unrelated work may continue, but the framework must not claim recovery from a native failure it
cannot safely contain.

Errors and status should identify the affected patch or component, the failure phase, and the useful cause. Expected
failures should use explicit results. Exceptions must not cross native or plugin boundaries, and assertions should
represent internal invariants rather than replace runtime error handling. FusionCutter can contain failures it can
inspect and control; it is not a sandbox for arbitrary native plugin code.

### Logging and diagnostics

Logging should make the framework practical to troubleshoot without requiring a debugger for every problem. Important
lifecycle events, decisions, and failures should be visible at an appropriate log level, not only when a critical error
occurs.

Each component should log the work it owns with enough context to identify what happened and where. Debug messages
should help development and testing, warnings should identify unexpected or degraded behavior, and errors should
explain failed operations. Use the shared logging facilities so scope and severity remain consistent across the
framework.

Useful logging does not mean logging every call. Messages should explain meaningful behavior without flooding output
or adding unreasonable cost to runtime paths.

## Making changes

Preserve externally meaningful behavior unless a change is deliberate and reviewed. Private implementation details
may change when behavior and ownership remain correct.

Keep changes focused and complete. Treat unclear ownership, lifecycle, failure, or public behavior as a design question
instead of silently turning an implementation choice into a project rule. Resolve it before the choice becomes
difficult to reverse.
