#include <fc/plugin.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>

namespace {

// Exercises logging and tracing from a separately compiled plugin through the runtime's reporting API.
class ReportingFixture {
  public:
    // Submits scalar and text arguments, then overwrites the text before deferred formatting.
    explicit ReportingFixture(const fc::CreateContext &create) : log_(create.log()) {
        std::string text = "Temporary DLL text";
        const std::string_view view{text};
        log_.warning("DLL inputs: {} {} {:.1f} {} {}", 42, true, 2.5, text, view);
        // The test pauses the reporting worker so it cannot format the record before this overwrite.
        text.assign(text.size(), 'x');
    }

    // Reserves a channel and submits typed and byte-span records; failure rejects fixture preparation.
    fc::Result<> prepare(const fc::PrepareContext &prepare) {
        auto result = prepare.trace({"SDK records", 5, 65536});
        if (!result) {
            return std::unexpected(result.error());
        }

        const auto trace = *result;
        const std::uint32_t integer = 0x12345678;
        std::array<std::byte, 3> bytes{std::byte{1}, std::byte{2}, std::byte{3}};
        const auto first = trace.write(integer);
        const auto second = trace.write(std::span<const std::byte>{bytes});

        // The emitted trace must retain the submitted bytes after this local buffer changes.
        bytes.fill(std::byte{9});
        if (!first || !second) {
            return std::unexpected(fc::Error{"The reporting fixture could not submit both trace records"});
        }

        log_.write(fc::LogLevel::Warning, "DLL preformatted text");
        return {};
    }

  private:
    fc::Logger log_;
};

// Declares the fixture patch for the synthetic game identity used by the reporting test.
fc::Plugin make_plugin() {
    return {.id = "ReportingFixture",
            .version = "1.0",
            .patches = {fc::Patch{.id = "Output", .enabled = true, .variants = {fc::variant<ReportingFixture>({"Fixture.Game"})}}}};
}
} // namespace
FC_PLUGIN(make_plugin)
