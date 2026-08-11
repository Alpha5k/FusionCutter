#include "colored_chats.hpp"

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace fusioncutter::patches::colored_chats {
namespace {

constexpr std::size_t kChatTextCapacity = 128;
constexpr std::uintptr_t kFirstArgumentOffset = 0x08;
constexpr std::uintptr_t kColorLocalOffset = 0x1C;

constexpr std::uint32_t kAdminSender = 0x40;
constexpr std::uint32_t kTeamChannel = 1;
constexpr std::uint32_t kOpaqueAlpha = 0xFF00'0000;
constexpr std::uint32_t kNativeAllyColor = 0xFF01'56D5;
constexpr std::uint32_t kNativeEnemyColor = 0xFFDF'2020;

constexpr std::uint32_t kAdminCheckOffset = 0x008B;
constexpr std::uint32_t kChannelCheckOffset = 0x01C9;
constexpr std::uint32_t kColorHookOffset = 0x021E;

constexpr auto kQueuePrologue = byte_array<0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x24>();
constexpr auto kDisplayPrologue = byte_array<0x55, 0x8B, 0xEC, 0x81, 0xEC, 0x5C, 0x01, 0x00, 0x00>();
constexpr auto kAdminCheck = byte_array<0x8B, 0x55, 0x08, 0x83, 0x3A, 0x40, 0x75, 0x7D>();
constexpr auto kChannelCheck = byte_array<0x8B, 0x45, 0x08, 0x8B, 0x48, 0x04, 0x3B, 0x4D, 0xE8, 0x75, 0x35>();
constexpr auto kColorHook = byte_array<0x51, 0x8B, 0xCC, 0x8D, 0x45, 0xE4, 0x50>();
constexpr auto kSteamGameMessageCall = byte_array<0xE8, 0xA7, 0x34, 0xFE, 0xFF>();
constexpr auto kGogGameMessageCall = byte_array<0xE8, 0xC7, 0x34, 0xFE, 0xFF>();
constexpr auto kKillMessageCall = byte_array<0xE8, 0x0A, 0xFE, 0xFF, 0xFF>();

struct TargetData {
    std::uint32_t queue_rva;
    std::uint32_t display_rva;
    std::uint32_t game_message_call_rva;
    std::uint32_t kill_message_call_rva;
};

struct ChatMessageHeader {
    std::uint32_t sender;
    std::uint32_t channel;
};

struct ColorSetting {
    std::string_view key;
    const std::string& text;
    std::uint32_t& output;
};

[[nodiscard]] constexpr std::uint32_t pack_color(std::uint32_t rgb) noexcept {
    return kOpaqueAlpha | rgb;
}

static_assert(pack_color(0x12'AB'EF) == 0xFF12'AB'EF);

constexpr TargetData kSteamTarget{0x001D13C0, 0x001D1570, 0x001C0534, 0x001A3BD1};
constexpr TargetData kGogTarget{0x001D2340, 0x001D24F0, 0x001C14C4, 0x001A4B81};

[[nodiscard]] constexpr TargetData target_data(TargetLayout layout) noexcept {
    switch (layout) {
    case TargetLayout::SteamRetail:
        return kSteamTarget;
    case TargetLayout::GOGRetail:
        return kGogTarget;
    case TargetLayout::Aspyr:
    case TargetLayout::ModTools:
        std::unreachable();
    }
    std::unreachable();
}

[[nodiscard]] const wchar_t* sanitize_chat_text(const wchar_t* text,
                                                std::array<wchar_t, kChatTextCapacity>& sanitized) noexcept {
    // r130 changed a shared string-copy routine; limit its one-line behavior to queued chat messages.
    bool changed{};
    for (std::size_t index = 0; index < sanitized.size() - 1; ++index) {
        const auto character = text[index];
        if (character == L'\0') {
            return changed ? sanitized.data() : text;
        }

        sanitized[index] = character == L'\n' ? L' ' : character;
        changed |= sanitized[index] != character;
    }
    return sanitized.data();
}

[[nodiscard]] std::expected<std::uint32_t, OutcomeReason> parse_color(std::string_view key,
                                                                      std::string_view text) noexcept {
    std::uint32_t rgb{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), rgb, 16);
    if (text.size() != 6 || error != std::errc{} || end != text.data() + text.size()) {
        return std::unexpected(
            OutcomeReason{std::string(key) + " must contain exactly six hexadecimal RGB digits", {}, {}});
    }

    // The renderer consumes packed AARRGGBB while configuration supplies RRGGBB.
    return pack_color(rgb);
}

} // namespace

std::expected<void, OutcomeReason> validate_colors(ColoredChatsSettings& settings) noexcept {
    const std::array<ColorSetting, 5> color_settings{{
        {"DefaultColor", settings.default_color, settings.colors.default_color},
        {"TeamColor", settings.team_color, settings.colors.team_color},
        {"AdminColor", settings.admin_color, settings.colors.admin_color},
        {"AllyColor", settings.ally_color, settings.colors.ally_color},
        {"EnemyColor", settings.enemy_color, settings.colors.enemy_color},
    }};

    for (const auto& setting : color_settings) {
        auto color = parse_color(setting.key, setting.text);
        if (!color.has_value()) {
            return std::unexpected(std::move(color.error()));
        }
        setting.output = *color;
    }
    return {};
}

ColoredChats::ColoredChats(ColoredChatsSettings settings, const TargetContext& target) noexcept
    : colors_(settings.colors), layout_(target.layout) {}

void ColoredChats::build_plan(PatchPlan& plan) {
    const auto target = target_data(layout_);
    plan.require_bytes("Verify chat display frame", target.display_rva, BytePattern::exact(kDisplayPrologue));
    plan.require_bytes("Verify admin chat field", target.display_rva + kAdminCheckOffset,
                       BytePattern::exact(kAdminCheck));
    plan.require_bytes("Verify team chat field", target.display_rva + kChannelCheckOffset,
                       BytePattern::exact(kChannelCheck));

    original_queue_ = plan.inline_hook("Keep queued chat text on one line", target.queue_rva,
                                       BytePattern::exact(kQueuePrologue), &ColoredChats::queue_chat_message);
    plan.mid_hook("Color team and admin chat", target.display_rva + kColorHookOffset, BytePattern::exact(kColorHook),
                  &ColoredChats::color_chat_message);
    const auto game_message_call = layout_ == TargetLayout::SteamRetail ? kSteamGameMessageCall : kGogGameMessageCall;
    original_game_message_ = plan.redirect_call_with_original(
        "Color general game-event messages", target.game_message_call_rva, BytePattern::exact(game_message_call),
        reinterpret_cast<AddGameMessage>(&ColoredChats::color_game_message));
    plan.redirect_call("Color kill-feed messages", target.kill_message_call_rva, BytePattern::exact(kKillMessageCall),
                       reinterpret_cast<AddGameMessage>(&ColoredChats::color_game_message));
}

void ColoredChats::enable_runtime() noexcept {
    active_.publish(*this);
}

void ColoredChats::disable_runtime() noexcept {
    active_.clear(*this);
}

void __cdecl ColoredChats::queue_chat_message(std::uint32_t destination, std::uint32_t sender,
                                              std::uint32_t recipients_low, std::uint32_t recipients_high,
                                              std::uint32_t text_key, const wchar_t* text,
                                              std::uint32_t channel) noexcept {
    const auto original = original_queue_.get();
    if (original == nullptr) {
        return;
    }
    if (text == nullptr) {
        original(destination, sender, recipients_low, recipients_high, text_key, text, channel);
        return;
    }

    std::array<wchar_t, kChatTextCapacity> sanitized{};
    original(destination, sender, recipients_low, recipients_high, text_key, sanitize_chat_text(text, sanitized),
             channel);
}

void ColoredChats::color_chat_message(MidHookContext& context) noexcept {
    const auto* patch = active_.read();
    if (patch == nullptr || context.ebp < kColorLocalOffset) {
        return;
    }

    std::uintptr_t message_address{};
    std::memcpy(&message_address, reinterpret_cast<const void*>(context.ebp + kFirstArgumentOffset),
                sizeof(message_address));
    if (message_address == 0) {
        return;
    }

    ChatMessageHeader message{};
    std::memcpy(&message, reinterpret_cast<const void*>(message_address), sizeof(message));

    auto color = patch->colors_.default_color;
    if (message.sender == kAdminSender) {
        color = patch->colors_.admin_color;
    } else if (message.channel == kTeamChannel) {
        color = patch->colors_.team_color;
    }

    // The native display routine copies this stack-local color immediately after the hook.
    std::memcpy(reinterpret_cast<void*>(context.ebp - kColorLocalOffset), &color, sizeof(color));
}

void __fastcall ColoredChats::color_game_message(void* display, void*, const wchar_t* text,
                                                 std::uint32_t color) noexcept {
    const auto original = original_game_message_.get();
    if (original == nullptr) {
        return;
    }

    if (const auto* patch = active_.read(); patch != nullptr) {
        switch (color) {
        case kNativeAllyColor:
            color = patch->colors_.ally_color;
            break;
        case kNativeEnemyColor:
            color = patch->colors_.enemy_color;
            break;
        default:
            break;
        }
    }

    original(display, text, color);
}

} // namespace fusioncutter::patches::colored_chats
