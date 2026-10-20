#include <fc/plugin.hpp>

namespace example {

struct Settings {
    int value = 7;
};

// Retains the configured value for the runtime's status report.
struct Handler {
    using Settings = example::Settings;
    int value;

    Handler(const fc::CreateContext &, const Settings &settings) : value(settings.value) {}

    // Reports the configured value through the public status interface.
    void write_status(fc::StatusContext &status) const noexcept {
        status.line("Example value: {}", value);
    }
};

// Declares a handler with settings for the BF2 images recognized by either runtime architecture.
fc::Patch status_patch() {
    return fc::Patch{.id = "Status",
                     .enabled = true,
                     .settings =
                         fc::settings<Settings>({fc::setting("Value", &Settings::value, "Value displayed in status.").range(1, 20)}),
                     .variants = {fc::variant<Handler>({"BF2.*"})}};
}

} // namespace example
