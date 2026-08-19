# FusionCutter

FusionCutter is a modular patching framework for *Star Wars Battlefront II* (2005). It provides a stable SDK for
independently built plugins while retaining a small built-in core contribution and target-specific loaders.

The framework is being rebuilt in staged component order. The repository may not contain every final target while a
stage is in progress; new directories are added only when their first real implementation files exist.

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

Until an implementation stage adds production or test targets, these commands validate only the repository build
shell and its development targets.

## License

FusionCutter is licensed under the [MIT License](LICENSE). Vendored dependencies retain their own licenses.
