#include "plugin.hpp"

#include <fc/native.hpp>
#include <fc/plugin.hpp>

#include <stdexcept>

namespace {

VerifierCounters counters;

// Returns the recognized game build's data offset for the fixture read.
std::uint32_t data_rva(const fc::Plan &plan) {
    const auto profile = plan.profile();
    if (profile.starts_with("BF2.Classic.")) {
        return 0x639010;
    }
    if (profile.starts_with("BF2.GOG.")) {
        return 0x3df010;
    }
    if (profile.starts_with("BF2.ModTools.")) {
        return 0x6c3010;
    }
    return 0x3de010;
}

struct Settings {
    std::uint32_t value = 7;
};

// Reads mapped file bytes and exposes whether offline validation called a live lifecycle stage.
class Ready {
  public:
    using Settings = ::Settings;

    // Records the settings passed by the runtime when it constructs this handler.
    Ready(const fc::CreateContext &, const Settings &settings) {
        ++counters.ready_constructed;
        counters.ready_value = settings.value;
    }

    // Collects an image read and continuing allocation access without running native game code.
    void plan(fc::Plan &plan) {
        ++counters.ready_planned;
        if (auto value = plan.read<std::uint8_t>(plan.at(data_rva(plan), "Fixture data"))) {
            counters.ready_observed = *value;
        }

        // Continuing access to allocated data has no image/RVA location to report.
        auto data = plan.allocate<std::uint32_t>(4);
        plan.access<std::uint32_t>(data);
    }

    // Records unexpected live preparation during offline validation.
    fc::Result<> prepare(fc::PrepareContext &) {
        ++counters.prepare;
        return {};
    }

    // Records unexpected live activation during offline validation.
    void activate(fc::ActivateContext &) noexcept {
        ++counters.activate;
    }

    // Records unexpected live updates during offline validation.
    void update(const fc::UpdateContext &) noexcept {
        ++counters.update;
    }
};

class Fail {
  public:
    // Retains one operation, then fails with deliberate JSON escaping test data.
    void plan(fc::Plan &plan) {
        static_cast<void>(plan.read<std::uint8_t>(data_rva(plan)));
        throw std::runtime_error("Fixture \"Plan\" failed\\with newline\ncontrol\t\x01 and UTF-8: \xc3\xa9");
    }
};

// Supplies successful planning and an optional failure through the real plugin boundary.
fc::Plugin make_plugin() {
    return {.id = "VerifyFixture",
            .version = "1",
            .patches = {
                fc::Patch{.id = "Ready",
                          .enabled = true,
                          .settings = fc::settings<Settings>(
                              {fc::setting("value", &Settings::value, "Value recorded when the fixture handler is constructed.")}),
                          .variants = {fc::variant<Ready>({"BF2.*"})}},
                fc::Patch{.id = "Fail", .enabled = false, .variants = {fc::variant<Fail>({"BF2.*"})}},
            }};
}
} // namespace

FC_PLUGIN(make_plugin)

// Returns cumulative observations; tests compare snapshots because admitted plugin DLLs remain loaded.
extern "C" __declspec(dllexport) void __cdecl FC_VerifierCounters(VerifierCounters *out) {
    if (out) {
        *out = counters;
    }
}
