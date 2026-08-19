# Contributing to Fusion Cutter

Thank you for helping improve Fusion Cutter. The project values readable, reviewable native code over clever or highly generalized implementations.

## Core requirements

These requirements govern implementation and review throughout the repository.

- Apply the project priorities in order: human readability and maintainability, coverage and expandability, safety, performance, then simplicity.
- Preserve the source ownership boundaries: `src/` contains only `core/`, `loaders/`, and `patches/`. Place shared code with the responsibility that owns it.
- Treat a changed or unknown binary as unsupported until it has a reviewed layout. Recognize targets through fixed metadata and validate every native site used by a selected patch.
- Build thin loaders around one shared core. Universal x86 and x64 cores are the default; role-specific cores are filtered builds of the same source and ABI.
- Implement features as compiled source patches with stable identities and explicit catalog construction. Use the generated compile-time catalog as the sole patch registration and discovery mechanism. Keep one logical identity for target-specific variants of the same feature.
- Keep patch-private addresses, preimages, ABI assumptions, validation, and runtime behavior with the owning patch. Register each patch through its `patch.cmake` manifest and `PatchDefinition`.
- Route every installation write and hook through the core patching operations so validation, range ownership, conflict detection, commit, rollback, memory protection, and instruction-cache handling remain coordinated.
- Keep initialization and runtime flow visible and bounded, with substantial initialization outside the Windows loader lock. Resolve configuration and patch selection during startup, then keep installed patches and runtime services active for the process lifetime.
- Keep failures local to the affected patch and its dependents. Promote startup-required or rollback failures to fatal, and preserve unrelated successful patches.
- Add a shared abstraction only when a demonstrated project requirement makes recurring behavior safer or clearer.
- Keep configuration, reporting, status, and mandatory crash capture core-owned. Patches contribute typed metadata, lifecycle behavior, log events, and bounded status snapshots.
- Use C++23, CMake 3.28+, Visual Studio 2022/v143, and the static CRT. Pin reviewed dependencies under `vendor/` and expose vendor functionality through project-owned adapters and interfaces.
- Focus tests on safety, consequential shared or complex behavior, artifact boundaries, boundedness, and reproduced defects. Keep all correctness checks runnable locally.
- Preserve the loader seams: one optional DirectInput chain, the RconServer shim, and the Classic Collection `GameWinMain` proxy. Keep crash capture in process and produce the core-owned human-readable text report.

## Repository expectations

- Keep each change focused and preserve unrelated work.
- Treat `vendor/`, generated build output, and staged files under `dist/` as read-only inputs or outputs of their owning workflows.

## Formatting

The checked-in formatting files are the mechanical sources of truth:

- `.clang-format` defines C and C++ layout and requires clang-format major version 22.
- `.editorconfig` defines editor-visible whitespace, encoding, indentation, and line endings.
- `.gitattributes` normalizes committed text files.

Format project-owned C and C++ files, then verify them with:

```powershell
./tools/format.ps1
./tools/format.ps1 -Check
```

The script covers project-owned files and leaves vendored, generated, and distribution files unchanged.

Use the formatter, editor integration, automation, and review to enforce formatting and naming conventions. Reserve build, catalog, runtime, and test rejection for text that must be parsed, generated, compiled, or otherwise consumed correctly.

## Rules beyond the formatter

| Construct | Convention | Example |
|---|---|---|
| Types and enum members | `PascalCase` | `TargetContext` |
| Functions, variables, and namespaces | `snake_case` | `build_plan` |
| Private data members | trailing-underscore `snake_case_` | `runtime_state_` |
| Constants | `kPascalCase` | `kMaximumReportSize` |
| Files and directories | `snake_case` | `direct_transport.cpp` |
| C ABI declarations | project `FC_` spelling | `FC_INIT_FATAL` |
| Project macros | `FC_UPPER_SNAKE_CASE` | `FC_LOG_ERROR` |

- Give C++ headers the `.hpp` extension and C-compatible boundary headers the `.h` extension. Use `#pragma once`. Group closely related small declarations when that keeps the interface clear, and split files along meaningful responsibilities.
- Keep public declarations in `include/FusionCutter/<component>.hpp`. Keep small template implementations with the public interface; place substantial template implementation in `include/FusionCutter/templates/<component>.hpp` and include it from the public header.
- Include the corresponding header first, followed by project, third-party, platform, and standard-library groups. Include what the file directly uses and preserve required platform ordering, such as `WinSock2.h` before `Windows.h`.
- Treat replacement of inline assembly, naked functions, custom trampolines, or manual stack handling as an ABI change. Verify the replacement against both the original implementation and target machine code.
- Document every significant project-owned class or service and each nontrivial function that represents a distinct framework phase, feature step, lifecycle action, native integration point, protocol operation, concurrency boundary, or safety boundary. Put a short role comment above its declaration, or above its definition when it has no separate declaration. A reader should be able to scan the declarations and understand the component's responsibilities and flow without reading every function body.
- Document public SDK and plugin-boundary APIs with the purpose and any non-obvious ownership, lifetime, threading, or failure contract an author needs to use them correctly. Keep that contract with the public declaration.
- Simple constructors, destructors, accessors, direct forwarding functions, and self-evident value conversions do not need individual comments. Closely related trivial declarations may share one group comment.
- Use role comments to summarize what code contributes to the patch or game behavior. Use inline comments to explain why: reverse-engineered behavior, the defect being fixed, ABI or layout assumptions, safety constraints, and non-obvious decisions. Keep comments concise and avoid narrating statements line by line.
- Explain state whose units, ownership, synchronization, or invariants are not clear from its name and type. Keep the explanation near the declaration it governs.
- Prefer short comments that make nearby code easier to read. Do not repeat identifiers in sentence form or paste large design explanations beside ordinary code; put genuinely long reasoning in a focused design or reverse-engineering document.
- Keep comments current as behavior changes. Treat an inaccurate comment as a defect.
- Reserve `clang-format off/on` for a narrow native byte, instruction, or layout representation whose readability depends on its manual layout, and state that reason beside the directive.
- Prefer C++23 language and standard-library facilities when they provide equivalent behavior and safety. Use custom implementations for measured hot paths and constrained native or crash-reporting work where they provide a concrete benefit.
- Write idiomatic modern C++ and use RAII. Use C-style constructs at C ABI, native interface, and binary-layout boundaries where the constraint requires them.
- Treat duplicated logic, deeply nested control flow, and large multi-responsibility functions or structures as review signals. Refactor when the result is measurably easier to understand while preserving correctness, safety, native compatibility, and required performance.
- Prefer explicit ownership and visible control flow. Use a wrapper when it makes recurring complex behavior safer or clearer.
- Represent expected target, configuration, resource, and network failures with explicit results. Use assertions for programmer invariants. Contain exceptions within internal implementation and translate them before ABI or host entry points. Use SEH only for a narrow, documented expected-fault scope with a safe fallback.
- Keep hook and game-thread callbacks `noexcept`, bounded, and prepared before activation. Limit them to bounded in-memory work; place file I/O, blocking waits, recurring lookup, and other independently paced work in prepared patch-owned services.

## Before submitting a change

- Run `./tools/format.ps1 -Check`.
- Build the affected architecture, role, and configuration with the checked-in CMake presets.
- Run the most focused available tests that cover the change.
- For every new or materially changed component, review its declarations, public contracts, nontrivial entry points, and important state for the required comments.
- Document any validation that could not be performed.
- Update user or contributor documentation when behavior or supported usage changes.
