#pragma once

#include <FusionCutter/patch.hpp>

#include <cstdint>
#include <expected>
#include <string>

namespace fusioncutter::patches::colored_chats {

struct MessageColors {
    std::uint32_t default_color{};
    std::uint32_t team_color{};
    std::uint32_t admin_color{};
    std::uint32_t ally_color{};
    std::uint32_t enemy_color{};
};

struct ColoredChatsSettings {
    std::string default_color;
    std::string team_color;
    std::string admin_color;
    std::string ally_color;
    std::string enemy_color;
    MessageColors colors{};
};

// Converts every configured RGB value to the game's packed color representation.
[[nodiscard]] std::expected<void, OutcomeReason> validate_colors(ColoredChatsSettings& settings) noexcept;

// Colors chat and game-event messages while keeping queued chat text on one line.
class ColoredChats final : public RuntimePatch {
  public:
    ColoredChats(ColoredChatsSettings settings, const TargetContext& target) noexcept;

    void build_plan(PatchPlan& plan) override;
    void enable_runtime() noexcept override;
    void disable_runtime() noexcept override;

  private:
    using QueueChatMessage = void(__cdecl*)(std::uint32_t destination, std::uint32_t sender,
                                            std::uint32_t recipients_low, std::uint32_t recipients_high,
                                            std::uint32_t text_key, const wchar_t* text,
                                            std::uint32_t channel) noexcept;
    using AddGameMessage = void(__thiscall*)(void* display, const wchar_t* text, std::uint32_t color) noexcept;

    // Removes embedded newlines before a chat message enters the native queue.
    static void __cdecl queue_chat_message(std::uint32_t destination, std::uint32_t sender,
                                           std::uint32_t recipients_low, std::uint32_t recipients_high,
                                           std::uint32_t text_key, const wchar_t* text, std::uint32_t channel) noexcept;

    // Replaces the three chat-channel colors at the native display boundary.
    static void color_chat_message(MidHookContext& context) noexcept;

    // Replaces the game's ally and enemy event-message colors.
    static void __fastcall color_game_message(void* display, void*, const wchar_t* text, std::uint32_t color) noexcept;

    MessageColors colors_;
    TargetLayout layout_;

    inline static PatchInstanceSlot<ColoredChats> active_;
    inline static OriginalFunction<QueueChatMessage> original_queue_;
    inline static OriginalFunction<AddGameMessage> original_game_message_;
};

} // namespace fusioncutter::patches::colored_chats
