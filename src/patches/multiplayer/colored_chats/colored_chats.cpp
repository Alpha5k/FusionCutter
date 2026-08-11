#include "colored_chats.hpp"

#include <array>
#include <bit>
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

constexpr std::uint32_t kAdminCheckOffset = 0x008B;
constexpr std::uint32_t kChannelCheckOffset = 0x01C9;
constexpr std::uint32_t kColorHookOffset = 0x021E;

constexpr auto kQueuePrologue = byte_array<0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x24>();
constexpr auto kDisplayPrologue = byte_array<0x55, 0x8B, 0xEC, 0x81, 0xEC, 0x5C, 0x01, 0x00, 0x00>();
constexpr auto kAdminCheck = byte_array<0x8B, 0x55, 0x08, 0x83, 0x3A, 0x40, 0x75, 0x7D>();
constexpr auto kChannelCheck = byte_array<0x8B, 0x45, 0x08, 0x8B, 0x48, 0x04, 0x3B, 0x4D, 0xE8, 0x75, 0x35>();
constexpr auto kColorHook = byte_array<0x51, 0x8B, 0xCC, 0x8D, 0x45, 0xE4, 0x50>();

struct TargetData {
    std::uint32_t queue_rva;
    std::uint32_t display_rva;
};

struct ChatMessageHeader {
    std::uint32_t sender;
    std::uint32_t channel;
};

constexpr TargetData kSteamTarget{0x001D13C0, 0x001D1570};
constexpr TargetData kGogTarget{0x001D2340, 0x001D24F0};

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

    // RedColor stores opaque RGBA bytes; configuration uses the conventional RRGGBB order.
    return std::byteswap((rgb << 8) | 0xFF);
}

} // namespace

std::expected<void, OutcomeReason> validate_colors(ColoredChatsSettings& settings) noexcept {
    auto color = parse_color("DefaultColor", settings.default_color);
    if (!color.has_value()) {
        return std::unexpected(std::move(color.error()));
    }
    settings.colors.default_color = *color;

    color = parse_color("TeamColor", settings.team_color);
    if (!color.has_value()) {
        return std::unexpected(std::move(color.error()));
    }
    settings.colors.team_color = *color;

    color = parse_color("AdminColor", settings.admin_color);
    if (!color.has_value()) {
        return std::unexpected(std::move(color.error()));
    }
    settings.colors.admin_color = *color;
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

} // namespace fusioncutter::patches::colored_chats
