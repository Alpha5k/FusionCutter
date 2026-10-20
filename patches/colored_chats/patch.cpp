#include "patch.hpp"

#include "messages.hpp"

#include <fc/native.hpp>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace fc::core_plugin::colored_chats {
namespace {

// Stores RGB settings validated independently by the SDK.
struct Options {
    std::string chat = "DFDF20", team = "20DF20", admin = "D8D8D8", buddy = "10F9CA";
};

// The queue copies text synchronously and stores a 64-bit recipient mask in each native record.
using QueueChatMessage = void(FC_CDECL *)(std::uint32_t destination, std::uint32_t sender, std::uint64_t recipients, std::uint32_t text_key,
                                          const wchar_t *text, std::uint32_t channel) noexcept;
// ECX is unused by the game; retaining it matches buddy hooks, while EDX carries the sender.
using IsPlayerBuddy = bool(__fastcall *)(std::int32_t local_player, std::int32_t player) noexcept;

// Matches the leading fields read by the display routine's admin and team checks.
struct ChatHeader {
    std::uint32_t sender, channel;
};

// Sanitizes queued chat and replaces the display routine's local color for those records.
class Handler {
  public:
    using Settings = Options;

    // Retains the native colors after the SDK has validated each setting separately.
    Handler(const fc::CreateContext &, const Settings &settings)
        : colors_{*parse_color(settings.chat), *parse_color(settings.team), *parse_color(settings.admin), *parse_color(settings.buddy)} {}

    // Validates the record fields and installs the queue and display callbacks for the selected client.
    void plan(fc::Plan &plan) {
        const std::uint32_t queue = plan.profile() == "BF2.Steam.59EDE353" ? 0x1d13c0 : 0x1d2340;
        plan.check(plan.at(queue + 0x23b, "Chat display admin sender check"), 6, fc::exact_bytes({0x8b, 0x55, 0x08, 0x83, 0x3a, 0x40}));
        plan.check(plan.at(queue + 0x379, "Chat display team channel check"), 9,
                   fc::exact_bytes({0x8b, 0x45, 0x08, 0x8b, 0x48, 0x04, 0x3b, 0x4d, 0xe8}));
        is_player_buddy_ =
            plan.function<IsPlayerBuddy>(plan.at(0x12e190, "FriendBuddyList::IsFriendBuddy"),
                                         fc::exact_bytes({0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf8, 0x51, 0x53, 0x56, 0x57, 0x8b, 0xda}));

        queue_ = plan.hook<QueueChatMessage>(
            plan.at(queue, "NetGame::AddChatMessageToQue"),
            [this](std::uint32_t destination, std::uint32_t sender, std::uint64_t recipients, std::uint32_t text_key, const wchar_t *text,
                   std::uint32_t channel) noexcept {
                // The original queue copies the bounded text before this invocation's buffer is destroyed.
                std::array<wchar_t, kChatCapacity> buffer{};
                static_cast<void>(queue_.call(destination, sender, recipients, text_key, one_line(text, buffer), channel));
            },
            fc::exact_bytes({0x55, 0x8b, 0xec, 0x83, 0xec, 0x24}));
        plan.hook_instruction(
            plan.at(queue + 0x3ce, "Chat display color"),
            [this](fc::CpuContext &context) noexcept {
                color_chat(context);
            },
            fc::exact_bytes({0x51, 0x8b, 0xcc, 0x8d, 0x45, 0xe4}));
    }

  private:
    // Reads the display record from its x86 frame and replaces the local color copied into AddMessage.
    void color_chat(fc::CpuContext &context) noexcept {
        const auto frame = fc::frame_pointer(context);

        // The first argument is a four-byte native pointer even in unused declarations compiled for x64.
        const auto message_address = fc::read_field<std::uint32_t>(frame, 8);
        if (!message_address) {
            return;
        }
        const auto message = fc::read_field<ChatHeader>(message_address);
        const bool buddy =
            message.sender != kAdminSender && is_player_buddy_.call(0, static_cast<std::int32_t>(message.sender)).value_or(false);
        const auto color = message_color(colors_, message.sender, message.channel, buddy);
        fc::write_field(frame, -0x1c, color);
    }

    Colors colors_;
    fc::NativeFunction<QueueChatMessage> queue_;
    fc::NativeFunction<IsPlayerBuddy> is_player_buddy_;
};

// Rejects one invalid RGB setting so the runtime warns and retains only that field's default.
fc::Result<> validate_color(std::string_view text) {
    if (!parse_color(text)) {
        return std::unexpected(fc::Error{"Color must be a six-digit RGB value"});
    }
    return {};
}

} // namespace

fc::Patch definition() {
    return fc::Patch{.id = "ColoredChats",
                     .description = "Set chat colors by message type.",
                     .category = "Client",
                     .enabled = true,
                     .settings = fc::settings<Options>(
                         {fc::setting("DefaultColor", &Options::chat, "Six-digit RGB color for normal chat.").validate(&validate_color),
                          fc::setting("TeamColor", &Options::team, "Six-digit RGB color for team chat.").validate(&validate_color),
                          fc::setting("AdminColor", &Options::admin, "Six-digit RGB color for admin chat.").validate(&validate_color),
                          fc::setting("BuddyColor", &Options::buddy, "Six-digit RGB color for buddy chat.").validate(&validate_color)}),
                     .variants = {fc::variant<Handler>({"BF2.Steam.59EDE353", "BF2.GOG.59EDF52B"}, fc::Role::Client)}};
}

} // namespace fc::core_plugin::colored_chats
