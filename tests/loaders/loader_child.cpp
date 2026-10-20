#include "child_support.hpp"

// Dispatches one harness scenario to the client loader, embedded runtime or production startup validation.
int main(int count, char **values) {
    // Return invalid-image errors directly to the fixture instead of opening Windows error dialogs.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    if (count != 2) {
        return 2;
    }

    const std::string_view scenario{values[1]};
    if (scenario == "runtime-boundary") {
        return fc::tests::loaders::runtime_boundary();
    }
#if defined(FC_TEST_DEDICATED)
    if (scenario == "combined" || scenario == "combined-bf1" || scenario == "combined-copy") {
        return fc::tests::loaders::combined(scenario);
    }
#endif
    return fc::tests::loaders::client(scenario);
}
