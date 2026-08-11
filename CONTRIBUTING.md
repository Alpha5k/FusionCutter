# Contributing to Fusion Cutter

Thank you for helping improve Fusion Cutter. The project values readable, reviewable native code over clever or highly generalized implementations.

## Core requirements

These requirements govern implementation and review throughout the repository.

- Apply the project priorities in order: human readability and maintainability, coverage and expandability, safety, performance, then simplicity.
- Keep Fusion Cutter independent from Prod, Dev, and BF2GameExt. Preserve selected patch behavior, the Direct Transport wire protocol, and SWBF2Admin compatibility unless a change is explicitly approved.
- Preserve the source ownership boundaries: `src/` contains only `core/`, `loaders/`, and `patches/`. Put shared code with the responsibility that owns it rather than adding miscellaneous common, shared, service, utility, platform, or duplicate API directories.
- Support only the reviewed Steam retail, GOG retail, Classic Collection, and Mod Tools layouts and roles. A changed or unknown binary requires a reviewed layout; do not add runtime signature discovery or cross-layout RVA fallback.
- Build thin loaders around one shared core. Universal x86 and x64 cores are the default; role-specific cores are optional filtered builds of the same source and ABI.
- Implement features as compiled source patches with stable identities and explicit catalog construction. One logical patch may own target-specific variants. Do not add runtime patch discovery or a binary patch-plugin ABI.
- Keep patch-private addresses, preimages, ABI assumptions, validation, and runtime behavior with the owning patch. Loaders do not enumerate patches, and static self-registration is prohibited.
- Patches describe work through the core patching operations. Do not bypass centralized validation, range ownership, conflict detection, commit, rollback, or memory-protection handling with direct game-memory writes.
- Keep the initialization path and runtime work visible and bounded, with substantial initialization outside the Windows loader lock. The model is startup-only; do not add general hot enable, disable, unload, configuration reload, restartable services, or recurring discovery.
- Keep failures local to the affected patch and its dependents unless a startup-required variant or rollback failure promotes initialization to fatal. Do not unwind unrelated successful patches.
- Do not introduce speculative service containers, event buses, schedulers, thread pools, generic managers, or other abstractions without a demonstrated project requirement.
- Keep configuration, reporting, status, and mandatory crash capture core-owned. Patches declare typed metadata and contributions instead of owning independent INI readers or output pipelines.
- Use the required C++23, CMake 3.28+, Visual Studio 2022/v143, static-CRT toolchain. Pin reviewed dependencies under `vendor/` and keep vendor types behind project-owned adapters and interfaces.
- Add tests for safety, consequential shared or complex behavior, artifact boundaries, boundedness, or reproduced defects—not for trivial declarations, syntax, or dependency-library behavior. All correctness checks must run locally; passing tests never permits violating project requirements.
- Preserve the loader seams. Do not add multiple DirectInput chains, minidumps, a separate crash reporter, an Aspyr launcher replacement, or alternate Classic injection/bootstrap machinery.

## Repository expectations

- Keep each change focused and preserve unrelated work.
- Do not edit `vendor/`, generated build output, or staged files under `dist/`.

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

The script excludes vendored, generated, and distribution files. Do not reformat third-party code.

Formatting and naming conventions are enforced by these tools, editor integration, automation, and review. Do not add build, catalog, runtime, or test gates solely to reject style violations; reject text only when it cannot be parsed, generated, compiled, or otherwise used correctly.

## Rules not enforced by the formatter

| Construct | Convention | Example |
|---|---|---|
| Types and enum members | `PascalCase` | `TargetContext` |
| Functions, variables, and namespaces | `snake_case` | `build_plan` |
| Private data members | trailing-underscore `snake_case_` | `runtime_state_` |
| Constants | `kPascalCase` | `kMaximumReportSize` |
| Files and directories | `snake_case` | `direct_transport.cpp` |
| C ABI declarations | project `FC_` spelling | `FC_INIT_FATAL` |
| Project macros | `FC_UPPER_SNAKE_CASE` | `FC_LOG_ERROR` |

- C++ headers use `.hpp`; C-compatible boundary headers use `.h`; headers use `#pragma once`. Closely related small declarations may share a header; there is no one-class-per-file rule.
- Keep public declarations in `include/FusionCutter/<component>.hpp`. When substantial template implementation would obscure that interface, place it in `include/FusionCutter/templates/<component>.hpp` and include it from the public header; do not split small templates mechanically.
- Include the corresponding header first, followed by project, third-party, platform, and standard-library groups. Include what the file directly uses and preserve required platform ordering, such as `WinSock2.h` before `Windows.h`.
- Use short comments to identify a file, class, function, or block's role when that role is not immediately clear from its name and interface. This is especially important when one patch contains several features or native integration points.
- Explain the game behavior being changed, the defect being fixed, native ABI or layout assumptions, safety constraints, and non-obvious decisions. Describe what the code establishes; avoid speculation or historical narrative.
- Do not restate readable code, repeat address provenance already expressed by the target layout, advertise the expected consumer, or justify ordinary implementation details.
- Keep comments beside the declaration or logic they clarify. Update or remove comments when behavior changes; an inaccurate comment is a defect.
- Use `clang-format off/on` only for a narrow native byte, instruction, or layout representation that formatting would obscure, and state the reason beside the directive.
- Prefer C++23 language and standard-library facilities over project-owned reimplementations when they provide equivalent behavior and safety. Marginal theoretical performance differences do not justify custom code outside measured hot paths or constrained native and crash-reporting work.
- Write idiomatic modern C++ and use RAII. Use C-style constructs only where a C ABI, native interface, binary layout, or other concrete constraint requires them.
- Treat duplicated logic, deeply nested control flow, and large multi-responsibility functions or structures as review signals, not automatic violations. Refactor only when the result is measurably easier to understand without weakening correctness, safety, native compatibility, or required performance.
- Prefer explicit ownership and visible control flow. A wrapper should make recurring complex behavior safer or clearer, not merely shorten the code.
- Expected target, configuration, resource, and network failures use explicit results. Assertions are for programmer invariants. Exceptions never cross ABI or host entry points. Do not use blanket SEH; a narrow documented expected-fault scope requires a safe fallback.
- Hook and game-thread callbacks are `noexcept`, bounded, and prepared before activation. They do not perform file I/O, blocking waits, repeated lookup, unbounded work, or avoidable allocation and synchronization.

## Before submitting a change

- Run `./tools/format.ps1 -Check`.
- Build the affected architecture, role, and configuration with the checked-in CMake presets.
- Run the most focused available tests that cover the change.
- Document any validation that could not be performed.
- Update user or contributor documentation when behavior or supported usage changes.
