#pragma once

#include <FusionCutter/patch.hpp>

#include <cstdint>
#include <expected>
#include <string>

namespace fusioncutter::patches::colored_chats {

struct ChatColors {
    std::uint32_t default_color{};
    std::uint32_t team_color{};
    std::uint32_t admin_color{};
};

struct ColoredChatsSettings {
    std::string default_color;
    std::string team_color;
    std::string admin_color;
    ChatColors colors{};
};

[[nodiscard]] std::expected<void, OutcomeReason> validate_colors(ColoredChatsSettings& settings) noexcept;

// Colors normal, team, and admin chat while keeping queued messages on one line.
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

    static void __cdecl queue_chat_message(std::uint32_t destination, std::uint32_t sender,
                                           std::uint32_t recipients_low, std::uint32_t recipients_high,
                                           std::uint32_t text_key, const wchar_t* text, std::uint32_t channel) noexcept;
    static void color_chat_message(MidHookContext& context) noexcept;

    ChatColors colors_;
    TargetLayout layout_;

    inline static PatchInstanceSlot<ColoredChats> active_;
    inline static OriginalFunction<QueueChatMessage> original_queue_;
};

} // namespace fusioncutter::patches::colored_chats
