#include "patch.hpp"

#include <fc/native.hpp>
#include <cstdint>

namespace fc::core_plugin::unlock_frame_rate {
namespace {

struct Settings {
    std::uint32_t maximum = 120;
};

// Replaces the two-byte PUSH with a full-width divisor in the function's preceding padding.
class Handler {
  public:
    using Settings = unlock_frame_rate::Settings;

    Handler(const fc::CreateContext &, const Settings &settings) : maximum_(settings.maximum) {}

    // Validates the divisor setup and padding before constructing the two short branches.
    void plan(fc::Plan &plan) {
        const bool modtools = plan.profile() == "BF2.ModTools.43EBD102";
        const std::uint32_t update_time = modtools ? 0x4e850 : plan.profile() == "BF2.Steam.59EDE353" ? 0x13ab20 : 0x13b880;
        const auto limit = plan.at(update_time + (modtools ? 0x77 : 0x5b), "Frame rate limit");
        const auto padding = plan.at(update_time - 7, "Frame rate code padding");

        // The native division takes a 64-bit divisor: retail pushes zero, while ModTools pushes a zeroed EDI.
        if (modtools) {
            plan.check(plan.at(update_time + 0x51, "Clear frame divisor high word"), 2, fc::exact_bytes({0x33, 0xff}));
            plan.check(plan.at(update_time + 0x76, "Frame divisor high word"), 1, fc::exact_bytes({0x57}));
        } else {
            plan.check(plan.at(update_time + 0x59, "Frame divisor high word"), 2, fc::exact_bytes({0x6a, 0x00}));
        }

        auto push_divisor = fc::byte_array<0x68, 0x00, 0x00, 0x00, 0x00, 0xeb, 0x00>();
        auto redirect = fc::byte_array<0xeb, 0x00>();
        // Short branches measure signed displacement from the end of each branch instruction.
        const auto limit_end = limit.rva + redirect.size();
        const auto padding_end = padding.rva + push_divisor.size();
        const auto outward = fc::relative_displacement<std::int8_t>(limit_end, padding.rva);
        const auto resume = fc::relative_displacement<std::int8_t>(padding_end, limit_end);
        if (!outward || !resume) {
            plan.fail("Frame rate branches exceed the short jump range");
            return;
        }

        // This unsigned divisor preserves the existing effectively uncapped timing without division by zero.
        const auto divisor = maximum_ == 0 ? std::uint32_t{0xffff'ff80} : maximum_;
        fc::write_value<1>(push_divisor, divisor);
        fc::write_value<6>(push_divisor, *resume);
        fc::write_value<1>(redirect, *outward);
        plan.write(padding, fc::Bytes{push_divisor}, fc::exact_bytes({0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc}));
        plan.write(limit, fc::Bytes{redirect}, fc::exact_bytes({0x6a, 0x50}));
    }

  private:
    std::uint32_t maximum_;
};

} // namespace

fc::Patch definition() {
    return fc::Patch{
        .id = "UnlockFrameRate",
        .description = "Set the game's maximum frame rate.",
        .category = "Client",
        .enabled = true,
        .settings = fc::settings<Settings>(
            {fc::setting("MaxFrameRate", &Settings::maximum, "Maximum rendered frames per second; 0 removes the limit.")}),
        .variants = {fc::variant<Handler>({"BF2.Steam.59EDE353", "BF2.GOG.59EDF52B", "BF2.ModTools.43EBD102"}, fc::Role::Client)}};
}

} // namespace fc::core_plugin::unlock_frame_rate
