#include "simple_write.hpp"

namespace fc::fixtures::simple_write {
namespace {

// The typed replacement setting proves configuration reaches the same handler for external and bundled acquisition.
struct Settings {
    std::uint32_t replacement = kDefaultReplacement;
};

// Retains the selected target and configured replacement until the framework requests the patch plan and status.
class Handler final {
  public:
    using Settings = simple_write::Settings;

    Handler(const fc::CreateContext& context, const Settings& settings) noexcept
        : target_(context.target()), replacement_(settings.replacement) {}

    // Submit one data write guarded by evidence; installation owns validation, page protection, and rollback.
    void plan(fc::Plan& plan) {
        plan.write(fc::DataLocation<std::uint32_t>{.rva = value_rva(target_),
                                                   .name = "SliceValue",
                                                   .evidence = fc::expect(kInitialValue)},
                   replacement_);
        plan.logger().debug("Planned the checked write at RVA 0x{:x} with replacement {}", value_rva(target_).value,
                            replacement_);
    }

    // Status makes the effective configured value observable without exposing private fixture handler state.
    void write_status(fc::StatusWriter& output) const noexcept {
        (void)output.add("Replacement", replacement_);
    }

  private:
    fc::TargetInfo target_;
    std::uint32_t replacement_{};
};

// Declares the single typed value generated, overridden, and delivered through the production settings path.
[[nodiscard]] fc::SettingsSchema<Settings> settings() {
    return fc::settings<Settings>(
        fc::value("Replacement", &Settings::replacement, kDefaultReplacement)
            .description("Unsigned value written after the original bytes pass evidence validation"));
}

// Client targets share one support because only their reviewed data RVAs differ.
[[nodiscard]] fc::Support client_support() {
    return fc::support<Handler>({.layouts = {fc::TargetLayout::GameSpyRetail, fc::TargetLayout::SteamRetail,
                                             fc::TargetLayout::ClassicCollection},
                                 .roles = fc::HostRole::Client,
                                 .image = fc::TargetImage::Game});
}

// Private native children use one coherent production server tuple per architecture with the same handler and plan.
[[nodiscard]] fc::Support server_support() {
    return fc::support<Handler>({.layouts = {fc::TargetLayout::GOGRetail, fc::TargetLayout::ClassicCollection},
                                 .roles = fc::HostRole::Server,
                                 .image = fc::TargetImage::Game});
}

} // namespace

fc::Plugin build_plugin() {
    return fc::plugin({
        .id = std::string{kPluginId},
        .version = "1.0.0",
        .patches =
            {
                fc::patch<Handler>({.id = std::string{kPatchId},
                                    .name = "Simple checked write",
                                    .enabled = true,
                                    .description = "Installs one configured write after validating the original value",
                                    .settings = settings(),
                                    .supports = {client_support(), server_support()}}),
            },
    });
}

} // namespace fc::fixtures::simple_write
