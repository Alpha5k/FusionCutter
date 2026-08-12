#include "unlock_frame_rate.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <utility>

namespace fusioncutter::patches::unlock_frame_rate {
namespace {

struct FrameLimiterCode {
    std::span<const std::byte> prefix;
    std::span<const std::byte> suffix;
    BytePattern code_cave_preimage;
};

struct FrameLimiterLayout {
    std::uint32_t frame_rate_rva;
    std::uint32_t code_cave_rva;
    const FrameLimiterCode& code;
};

constexpr auto kPushImmediate = byte_array<0x6A, 0x00>();
constexpr auto kFrameRateSlotMask = byte_array<0x7E, 0x00>();

constexpr auto kRetailPrefix = byte_array<0x6A, 0x00>();
constexpr auto kRetailSuffix = byte_array<0xFF, 0x35>();
constexpr auto kCompatibleCodeCave = byte_array<0x00, 0x00, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC>();
constexpr auto kCompatibleCodeCaveMask = byte_array<0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF>();

constexpr auto kModToolsPrefix = byte_array<0x57>();
constexpr auto kModToolsSuffix = byte_array<0x50, 0x51>();
constexpr auto kModToolsCodeCave = byte_array<0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC>();

constexpr FrameLimiterCode kRetailCode{kRetailPrefix, kRetailSuffix,
                                       BytePattern::masked(kCompatibleCodeCave, kCompatibleCodeCaveMask)};
constexpr FrameLimiterCode kModToolsCode{kModToolsPrefix, kModToolsSuffix, BytePattern::exact(kModToolsCodeCave)};

constexpr FrameLimiterLayout kSteamLayout{0x0013AB7B, 0x0013AB19, kRetailCode};
constexpr FrameLimiterLayout kGogLayout{0x0013B8DB, 0x0013B879, kRetailCode};
constexpr FrameLimiterLayout kModToolsLayout{0x0004E8C7, 0x0004E849, kModToolsCode};

constexpr std::uint32_t kUncappedDivisor = 0xFFFFFF80;

[[nodiscard]] std::array<std::byte, 2> short_jump(std::uint32_t instruction_rva,
                                                  std::uint32_t destination_rva) noexcept {
    auto jump = byte_array<0xEB, 0x00>();
    embed_relative_displacement<1, std::int8_t>(jump, instruction_rva + jump.size(), destination_rva);
    return jump;
}

[[nodiscard]] constexpr FrameLimiterLayout frame_limiter_layout(TargetLayout layout) noexcept {
    switch (layout) {
    case TargetLayout::SteamRetail:
        return kSteamLayout;
    case TargetLayout::GOGRetail:
        return kGogLayout;
    case TargetLayout::ModTools:
        return kModToolsLayout;
    case TargetLayout::Aspyr:
        std::unreachable();
    }
    std::unreachable();
}

} // namespace

UnlockFrameRate::UnlockFrameRate(UnlockFrameRateSettings settings, const TargetContext& target) noexcept
    : max_frame_rate_(settings.max_frame_rate), layout_(target.layout) {}

void UnlockFrameRate::build_plan(PatchPlan& plan) {
    const auto layout = frame_limiter_layout(layout_);
    const auto& code = layout.code;

    // Verify the surrounding 64-bit division without claiming the configurable operand itself.
    plan.require_bytes("Verify frame rate divisor high word",
                       layout.frame_rate_rva - static_cast<std::uint32_t>(code.prefix.size()),
                       BytePattern::exact(code.prefix));
    plan.require_bytes("Verify frame limiter continuation", layout.frame_rate_rva + kPushImmediate.size(),
                       BytePattern::exact(code.suffix));

    // The full-width form supports every configured value without PUSH imm8 sign extension above 127.
    auto extended_push = byte_array<0x68, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00>();

    // The reviewed uncapped executables use 0xFFFFFF80, which yields no wait without dividing by zero.
    const auto divisor = max_frame_rate_ == 0 ? kUncappedDivisor : max_frame_rate_;
    embed_value<1>(extended_push, divisor);

    const auto return_jump = short_jump(layout.code_cave_rva + 5, layout.frame_rate_rva + 2);
    embed_value<5>(extended_push, return_jump);

    plan.checked_write("Store extended frame rate limit", layout.code_cave_rva, code.code_cave_preimage, extended_push);

    const auto redirect = short_jump(layout.frame_rate_rva, layout.code_cave_rva);
    plan.checked_write("Use extended frame rate limit", layout.frame_rate_rva,
                       BytePattern::masked(kPushImmediate, kFrameRateSlotMask), redirect);
}

} // namespace fusioncutter::patches::unlock_frame_rate
