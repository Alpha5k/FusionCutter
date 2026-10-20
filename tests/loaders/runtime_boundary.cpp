#include "child_support.hpp"

namespace fc::tests::loaders {

int runtime_boundary() {
    const auto directory = stage();
    if (!copy_fixture(FC_PRODUCTION_CORE_PATH, directory / L"FusionCutter.dll")) {
        return 30;
    }

    const auto module = LoadLibraryW((directory / L"FusionCutter.dll").c_str());
    if (!module) {
        return 31;
    }

    // Call FC_Start directly so rejected requests can be inspected without the loader's fatal startup handling.
    using Start = std::uint32_t(FC_CDECL *)(std::uint32_t, const FC_StartOptions *, FC_ErrorSink);
    const auto start = resolve<Start>(module, "FC_Start");
    if (!start) {
        return 32;
    }

    // An ABI mismatch must be rejected before the deliberately invalid options pointer is read.
    StartError error;
    const FC_ErrorSink sink{&error, &StartError::receive};
    if (start(FC_ABI_VERSION + 1, reinterpret_cast<const FC_StartOptions *>(1), sink) != 0 || error.message.empty()) {
        return 33;
    }

    FC_StartOptions options{sizeof(options) - 1, 0};
    if (start(FC_ABI_VERSION, nullptr, sink) != 0 || start(FC_ABI_VERSION, &options, sink) != 0) {
        return 34;
    }

    options = {sizeof(options), 2};
    if (start(FC_ABI_VERSION, &options, sink) != 0 || error.message.empty()) {
        return 35;
    }
    return 0;
}

} // namespace fc::tests::loaders
