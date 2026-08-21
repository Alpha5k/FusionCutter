// A fixed exported value makes successful private dependency resolution observable during plugin registration.
extern "C" __declspec(dllexport) int fc_probe_dependency_value() noexcept {
    return 42;
}
