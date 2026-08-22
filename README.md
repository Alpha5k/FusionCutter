# FusionCutter

FusionCutter is a modular patching framework for *Star Wars Battlefront II* (2005). It provides a stable SDK for
independently built plugins while retaining a small built-in Core plugin and target-specific loaders.

FusionCutter is still early in development. Some targets and tooling remain incomplete, and releases should be treated
as development builds.

## Development

Before contributing, read:

- [CONTRIBUTING.md](CONTRIBUTING.md) for setup, build, test, and validation workflows;
- [STYLE_GUIDE.md](docs/STYLE_GUIDE.md) for source style, naming, organization, and comments; and
- [DESIGN_PRINCIPLES.md](docs/DESIGN_PRINCIPLES.md) for the project priorities and framework-wide principles.

Configure either supported architecture with the checked-in presets:

```powershell
cmake --preset vs2022-x86
cmake --build --preset vs2022-x86 --config Debug

cmake --preset vs2022-x64
cmake --build --preset vs2022-x64 --config Debug
```

## Plugin authoring

- [GETTING_STARTED.md](docs/GETTING_STARTED.md) builds a first plugin with the installed SDK.
- [SDK_REFERENCE.md](docs/SDK_REFERENCE.md) lists the public authoring and validation APIs.
- [BEST_PRACTICES.md](docs/BEST_PRACTICES.md) explains how to use the framework's facilities consistently.

## License

FusionCutter is licensed under the [MIT License](LICENSE). Vendored dependencies retain their own licenses.
