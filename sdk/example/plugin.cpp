#include <fc/plugin.hpp>

namespace example {

// Returns the example's configurable status handler.
fc::Patch status_patch();

// Declares the plugin exported through the public SDK boundary.
fc::Plugin build_plugin() {
    return {.id = "ExamplePlugin", .version = "1", .patches = {status_patch()}};
}

} // namespace example

FC_PLUGIN(example::build_plugin)
