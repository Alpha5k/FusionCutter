# FusionCutter Style Guide

This guide defines the source style for project-owned FusionCutter code. It covers how code is formatted, named,
organized, and commented. Architecture and behavior belong in the design specifications.

See [CONTRIBUTING.md](../CONTRIBUTING.md) for the development workflow and
[DESIGN_PRINCIPLES.md](DESIGN_PRINCIPLES.md) for project-wide design guidance.

The checked-in `.clang-format`, `.editorconfig`, and `.gitattributes` files are the mechanical authorities. This guide
records the conventions that tools cannot fully enforce. Vendored, generated, build, and distribution files retain
their own formatting.

## Formatting

### Text files

- Use UTF-8 without a byte-order mark.
- Use CRLF line endings, four-space indentation, and no tabs.
- End files with a newline and remove trailing whitespace.

### C and C++

FusionCutter uses clang-format major version 22.

| Action | Command |
| --- | --- |
| Format project-owned C and C++ | `./tools/format.ps1` |
| Check formatting without modifying files | `./tools/format.ps1 -Check` |

Use the following layout:

- Limit lines to 120 columns.
- Use attached braces.
- Place braces around every control-flow body, including single statements.
- Keep `else` on the same line as the preceding closing brace.
- Attach pointer and reference symbols to the type.
- Do not add indentation merely for namespace contents.
- Do not place nonempty functions or control statements on one line.

Let clang-format decide ordinary line wrapping and spacing. Do not manually pad unrelated declarations into aligned
columns; those layouts are fragile under later edits.

Use `clang-format off` and `clang-format on` only around a narrow representation of native instructions, bytes, or a
binary layout that is materially clearer when arranged by hand. Add a short comment explaining why the exception is
needed.

## Naming

### Conventions

| Construct | Convention | Example |
| --- | --- | --- |
| Types and enum members | `PascalCase` | `PatchState`, `Installed` |
| Functions, methods, fields, variables, and namespaces | `snake_case` | `build_installation_plan` |
| Private data members | `snake_case_` | `runtime_state_` |
| Constants | `kPascalCase` | `kMaximumReportSize` |
| Project files and directories | `snake_case` | `direct_transport.cpp` |
| Public C ABI names | established `FC_` forms | `FC_PluginApi`, `FC_INIT_FATAL` |
| Project macros | `FC_UPPER_SNAKE_CASE` | `FC_LOG_ERROR` |

Choose names that describe the value's actual role. Avoid abbreviations that make a name ambiguous, but do not repeat
context that is already clear from the containing type or namespace.

Treat acronyms as ordinary words in C++ names: `PluginApi`, `CpuContext`, and `Rva`, not `PluginAPI`, `CPUContext`, or
`RVA`. Preserve fixed external names and the established all-capital forms used by C ABI constants and macros.

### Terminology

Use one term for one concept, and preserve established project vocabulary instead of introducing a synonym.
Do not create shorthand by combining existing terms into a new label; describe the subject and relationship in ordinary
language instead.

Write comments, diagnostics, and test descriptions in clear, natural language that identifies the actual subject,
action, and purpose without assuming knowledge of internal terminology. Use a component, phase, or other named concept
only when that identity is relevant, and qualify it when necessary to make the meaning unambiguous. Prefer precise
descriptions such as "the framework," "the plugin catalog," or "the Prepare phase" over unexplained shorthand or
labels used as generic actors.

Use these suffixes consistently:

| Suffix | Meaning |
| --- | --- |
| `Result` | A fallible operation's returned success or error, or its detailed result |
| `Outcome` | A closed classification of a completed operation |
| `State` | Stored state that can change |
| `Phase` | A named lifecycle point used for attribution |
| `Record` | Retained or copied data with a clear owner |
| `Definition` | Declarative input |
| `Context` | Capabilities or input scoped to an operation or callback |
| `Request` | Input submitted to an operation |
| `Output` | Data produced by an operation |
| `Handle` | An opaque identity or a reference mediated by its owner |
| `Index` | A position in a named collection or domain |

Make units and domains clear when the type alone does not. For example, prefer `byte_size`, `rva`, or
`provider_patch` over a generic `size`, `offset`, or `provider` when the distinction matters.

## Source files

### Headers and file organization

- Use `.hpp` for C++ headers and `.h` for C-compatible boundary headers.
- Use `#pragma once`.
- Keep headers self-contained: each header includes the dependencies required by its declarations and can be included on
  its own.
- Use `snake_case` filenames unless an external ABI or artifact requires a fixed name.
- Group small related declarations when that makes the interface easier to scan. There is no one-class-per-file rule.
- Split files by meaningful responsibility, not by an arbitrary line count.
- Keep templates beside their declarations unless a separately included implementation header makes the public header
  materially easier to read.

### Includes

Include the file's corresponding header first. Follow it with readable groups in this order:

1. Project headers.
2. Third-party headers.
3. Platform headers.
4. Standard-library headers.

Separate groups with a blank line. Include what the file directly uses rather than relying on transitive includes.
Preserve required platform ordering, especially `WinSock2.h` before `Windows.h`. Includes are intentionally not sorted
automatically when doing so could disturb a required or more readable order.

Use quotes for the corresponding header and other local or private headers. Use angle brackets for public FusionCutter
headers included through their public path, third-party headers, platform headers, and standard-library headers.

## C++ usage

### General

Use idiomatic C++23 and RAII. Prefer standard-library facilities when they express the required behavior clearly.
Custom low-level facilities should have a concrete reason, such as an ABI, native integration, constrained execution
path, or measured performance requirement.

Use abstractions when they make ownership, intent, or repeated operations easier to understand. Prefer clear control
flow and well-named intermediate values over clever compression or dense expressions. Fewer lines are not an
improvement when the result is harder to read.

Keep C-style records and functions at a C ABI boundary or another boundary that exposes a binary layout. Ordinary C++
code should use ordinary C++ types and facilities. Use templates and concepts to express useful compile-time
constraints, not to hide straightforward logic behind unnecessary machinery.

### Namespaces

Use nested namespace syntax and anonymous namespaces for translation-unit-local declarations. Do not place a
`using namespace` directive in a header. Keep namespace aliases and using declarations within the narrowest useful
scope.

## Comments

Comments should let a reader understand the code without reconstructing its design history or tracing distant callers.
Explain what a unit accomplishes, why it exists or is structured a particular way, and the constraints or consequences
that matter. Comments supplement clear names and control flow; they do not replace them.

### Declaration comments

Comment types and functions unless their purpose and contract are routine and clear where they are declared. This
applies equally to internal functions, callbacks, test fixtures, and test helpers; being small or test-only does not
make a declaration self-explanatory. Document any important role, ownership, lifetime, threading, ordering, side
effect, or failure behavior that the signature does not convey.

One comment may document a group of closely related declarations when it clearly describes the purpose and contract
shared by the group. Add a file overview only when the file's purpose or scope would otherwise be unclear. Do not
repeat declaration comments at the definition.

### Implementation comments

Comment the major sections and transitions of nontrivial functions so their progression is clear without tracing every
statement. Comments are especially useful around distinct outcome paths, representation changes, non-obvious ordering,
and ownership, lifecycle, native, or failure boundaries. Place each comment immediately before the code it explains
and state the relevant purpose, reason, invariant, or consequence.

Native and reverse-engineered comments should preserve the relevant game behavior, ABI or layout assumption, and the
reason a location or operation is valid. Put member invariants, sentinel meanings, and lifetime or ordering constraints
beside the declarations they govern.

### Test comments

Test names state the behavior being checked; they do not replace comments explaining non-obvious fixture mechanics.
Comment fixture models, callback scripts, injected failures, deliberate state changes, lifecycle or teardown checks,
and groups of assertions whose relationship to the tested behavior is not apparent. Explain what a synthetic setup
represents and why it proves the intended behavior.

Do not label routine arrange, act, and assert steps or restate individual assertions. A test body whose setup, action,
and expected result are already obvious from its name and helpers does not need additional narration.

### Comment style

Use `//` and concise English with normal capitalization and punctuation. Comment a logical unit once; do not narrate
individual statements, restate visible code, or add comments at arbitrary intervals. An isolated fact or label is not
useful unless its relevance is clear. Remove commented-out code instead of preserving it as explanation.

When a short comment barely exceeds the line limit, prefer concise wording that keeps it on one line. Do not leave only
one or two words on a continuation line when the same meaning can be stated clearly in one line.

Keep long explanations in focused documentation, but make the local comment sufficient to understand the code's
purpose and the effect of the referenced rule or decision. Update comments with the code; an inaccurate comment is a
defect.
