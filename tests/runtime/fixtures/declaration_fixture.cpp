#include <fc/plugin.hpp>

#include <atomic>
#include <cstdint>
#include <string>

namespace {

// Tracks live handlers so tests can distinguish declaration loading from factory calls.
std::atomic_uint32_t active_handler_count{};

// Makes handler construction and destruction observable across the DLL boundary.
class FixtureHandler {
  public:
    // Counts construction through a fixture factory.
    FixtureHandler() {
        ++active_handler_count;
    }

    // Removes this handler from the count when its destroy callback runs.
    ~FixtureHandler() {
        --active_handler_count;
    }
};

// The byte-sized setting lets tests observe reconstruction and rejection of values above 255.
struct FixtureSettings {
    std::uint8_t value = 7;
};

// Retains the constructed setting for status assertions across the DLL boundary.
struct SettingsHandler {
    using Settings = FixtureSettings;
    std::uint8_t value;

    SettingsHandler(const fc::CreateContext &, const Settings &settings) : value(settings.value) {}

    // Writes the constructed setting as a decimal status line.
    void write_status(fc::StatusContext &status) const noexcept {
        status.line(std::to_string(value));
    }
};

// Combines declaration variants and a typed schema for runtime admission and factory tests.
fc::Plugin make_plugin() {
    return {
        .id = "fixture_plugin",
        .version = "1.2.3",
        .categories = {{.id = "general", .order = 0}},
        .groups = {{.id = "bundle", .description = "Fixture group.", .category = "general", .members = {"member"}, .enabled = true}},
        .patches = {fc::Patch{
                        .id = "main", .category = "general", .enabled = true, .variants = {fc::variant<FixtureHandler>({"Known.Game"})}},
                    fc::Patch{.id = "member",
                              .category = "general",
                              .enabled = false,
                              .configurable = false,
                              .variants = {fc::variant<FixtureHandler>({"Known.Game"})}},
                    fc::Patch{.id = "settings",
                              .enabled = true,
                              .settings = fc::settings<FixtureSettings>(
                                  {fc::setting("value", &FixtureSettings::value, "Fixture value.").range(1, 100)}),
                              .variants = {fc::variant<SettingsHandler>({"Known.Game"})}}},
    };
}

} // namespace

FC_PLUGIN(make_plugin)

// Lets tests observe whether declaration loading invoked a handler factory.
extern "C" __declspec(dllexport) std::uint32_t __cdecl FC_TestActiveHandlerCount() {
    return active_handler_count.load();
}
