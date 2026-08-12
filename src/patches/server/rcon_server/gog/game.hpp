#pragma once

#include <FusionCutter/patch.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>

namespace fusioncutter::patches::rcon_server::gog {

// Bridges RCON commands and notifications to the game's native server state.
class Game {
  public:
    static constexpr std::size_t kChatMessageCapacity = 512;
    static constexpr std::size_t kChatPumpLimit = 64;

    struct ChatMessage {
        std::array<char, kChatMessageCapacity> text{};
        std::size_t size{};
    };

    struct ChatDrainResult {
        std::size_t count{};
        bool empty{};
    };

    explicit Game(ImageContext image) noexcept;
    ~Game();

    // Validates native entry points and installs chat capture through the patch plan.
    void build_plan(PatchPlan& plan);

    // Verifies every game-owned data range used by the runtime service.
    [[nodiscard]] std::expected<void, OutcomeReason> validate() const;

    void enable() noexcept;
    void disable() noexcept;

    [[nodiscard]] std::uint16_t game_port() const noexcept;
    [[nodiscard]] std::string_view admin_password() const noexcept;
    [[nodiscard]] bool map_is_idle() const noexcept;
    // Executes a native RCON command or the added /lua command after readiness checks.
    [[nodiscard]] std::string execute(std::string_view command);

    // Drains captured chat into the service worker's bounded batch.
    [[nodiscard]] ChatDrainResult drain_chat(std::span<ChatMessage> output) noexcept;

  private:
    using Formatter = int(__cdecl*)(char*, std::size_t, const char*, ...);
    using LuaLoadBufferFunction = int(__cdecl*)(std::uint32_t, const char*, std::size_t, const char*);
    using LuaPcallFunction = int(__cdecl*)(std::uint32_t, int, int, int);
    using LuaSetTopFunction = void(__cdecl*)(std::uint32_t, int);

    static constexpr std::size_t kChatCapacity = 256;

    [[nodiscard]] bool server_ready(std::string_view command) const noexcept;
    [[nodiscard]] bool status_ready() const noexcept;
    [[nodiscard]] std::string execute_native(std::string_view command);
    [[nodiscard]] int execute_lua(std::string_view code) noexcept;
    void enqueue_chat(std::string_view message) noexcept;

    // Preserves native formatting while publishing the resulting chat line to the bounded queue.
    static int __cdecl capture_chat(char* buffer, std::size_t size, const char* format, ...) noexcept;

    static PatchInstanceSlot<Game> active_;
    static Formatter formatter_;

    ImageContext image_;
    std::array<ChatMessage, kChatCapacity> chat_queue_{};
    std::atomic_flag chat_lock_ = ATOMIC_FLAG_INIT;
    std::size_t chat_head_{};
    std::size_t chat_count_{};
};

} // namespace fusioncutter::patches::rcon_server::gog
