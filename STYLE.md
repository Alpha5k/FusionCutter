# Style

Use simple C++23 and standard library facilities where they fit. Keep abstractions small and tied to a concrete need.

## Names

| Item | Convention |
| --- | --- |
| Types and enum values | PascalCase |
| Functions, fields, variables, namespaces, source files and folders | snake_case |
| Private data members | Trailing underscore |
| Named constants | kPascalCase |
| Project macros | FC_UPPER_SNAKE_CASE |

Treat acronyms as words: `PluginApi`, `CpuContext`, `Rva`. Preserve external API names and conventional tool filenames.

Image profile constants preserve the profile's spelling, such as `kBF1SPTestFilenames` and `kBF2GOGSections`.
Keep build IDs in profile strings; distinguish multiple builds in variable names with readable versions or dates.

Fusion Cutter multiplayer feature IDs use `FC.PascalCase`, for example `FC.LuaMessages`.
Configuration option names use PascalCase, with explicit unit suffixes such as `MaxTraceSizeMB`.

## Formatting

Use clang-format 22 with the repository's `.clang-format` file. From the repository root:

```powershell
clang-format -i <files>
```

Separate definitions and logical steps with a single blank line. Keep related statements together and comments
beside the code they explain. Use UTF-8 without BOM, LF line endings and a final newline.

## C++

- Use `.hpp` for C++ headers, `.h` for C interfaces and `#pragma once`. Include the corresponding header first
  in implementation files. Headers include their own dependencies; public headers must not depend on private ones.
- Keep `using namespace` out of headers and implementation details local to their source files.
- Use scoped resource ownership and ordinary C++ internally, with the specified C representation at ABI boundaries.
- Select patch behavior by game build. Use architecture conditions for machine differences and architecture-specific loaders.
- Return expected failures through the normal error paths; reserve assertions for internal invariants.
- Use `[[nodiscard]]` for results that must be checked to avoid corrupting state, leaking resources or missing essential errors.
- Keep compiler warning settings local to project targets and explain warning suppressions.

## Prose

Use plain, consistent language throughout documentation and messages. Write settings and help for players;
write API documentation for patch authors.

Error messages should identify the failed operation and affected item, with useful details such as a path,
value or error code. Include the cause when it is known.

Keep setting descriptions to at most two short INI comment lines, including useful units and special values.
Use explicit `\n` line breaks; wrapping the C++ source does not wrap the generated description.

## Comments

- Document API contracts at their declarations, including ownership, lifetimes, units and thread requirements where relevant.
- Explain non-obvious algorithms, ordering and assumptions about the game beside the implementation.
- Put details about a field or operation beside that field or operation.
- Use `//` comments, usually one or two lines. Longer explanations are appropriate when the code needs them.
