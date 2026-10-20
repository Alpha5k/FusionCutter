#pragma once

#include "layout.hpp"

#include <fc/plugin.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace fc::core_plugin::rcon_server::gog {

// Connects RCON commands and chat notifications to the game's native functions and data.
class GameBridge {
  public:
    static constexpr std::size_t kChatMessageCapacity = 512;
    static constexpr std::size_t kChatPumpLimit = 64;

    // Holds one captured message and the byte count preceding its terminating NUL.
    struct ChatMessage {
        std::array<char, kChatMessageCapacity> text{};
        std::size_t size{};
    };

    // Reports one drained batch and whether the queue is empty afterward.
    struct ChatDrainResult {
        std::size_t count{};
        bool queue_empty{};
    };

    GameBridge() = default;

    GameBridge(const GameBridge &) = delete;
    GameBridge &operator=(const GameBridge &) = delete;

    // Declares native calls, memory access and completed chat observation for the selected layout.
    // This bridge must outlive the installed observer and the service worker that drains its messages.
    void plan(fc::Plan &plan, const Layout &layout);

    // Acquires direct views of the planned game data before the listener begins using it.
    fc::Result<> prepare();

    // Reads the configured game port after preparation.
    std::uint16_t direct_port() const noexcept;
    // Borrows the current password up to the native buffer's bound; prepare first and keep the game image loaded.
    std::string_view admin_password() const noexcept;
    // Reports whether the prepared mission state records a winning team.
    bool has_winner() const noexcept;

    // Executes a command; returns busy if not host, if a winner is declared, or if /status teams are missing.
    // Call serially after installation; native calls run on the caller's thread and use shared game state.
    std::string execute(std::string_view command);

    // Copies queued messages in order; a busy capture lock returns {0, false}, so emptiness remains unknown.
    ChatDrainResult drain_chat(std::span<ChatMessage> output) noexcept;
    // Counts messages discarded because the capture queue was full or its lock was busy.
    std::uint64_t dropped_chat_messages() const noexcept;

  private:
    static constexpr std::size_t kChatQueueCapacity = 256;

    // Requires host state, no mission winner and initialized teams for /status.
    bool server_ready(std::string_view command) const noexcept;
    // Checks the team records that NetAdmin's /status handler dereferences.
    bool status_ready() const noexcept;

    // Reproduces the native TCP RCON context and returns NetAdmin's bounded response text.
    std::string execute_native(std::string_view command);

    // Loads and calls one Lua buffer, restoring stack height; returns its Lua status or -1 for a missing state.
    int execute_lua(std::string_view code) noexcept;

    // Captures the reviewed formatter's stack buffer before its caller removes the cdecl arguments.
    void capture_chat(const fc::CpuContext &context) noexcept;

    // Copies one formatted message without allocating or waiting on the game thread.
    void enqueue_chat(std::string_view message) noexcept;

    // Buffers chat between the installed observer and the service worker.
    std::array<ChatMessage, kChatQueueCapacity> chat_queue_{};
    std::atomic_flag chat_lock_ = ATOMIC_FLAG_INIT;
    std::atomic_uint64_t dropped_chat_{};
    std::size_t chat_head_{};
    std::size_t chat_count_{};

    // Runtime handles retain the planned calls and memory ranges from the selected game image.
    fc::NativeFunction<RemoteAdminCall> command_parser_;
    fc::NativeFunction<LuaLoadBufferFunction> lua_load_;
    fc::NativeFunction<LuaProtectedCallFunction> lua_call_;
    fc::NativeFunction<LuaGetTopFunction> lua_get_top_;
    fc::NativeFunction<LuaSetTopFunction> lua_set_top_;
    fc::Memory<char> response_memory_;
    fc::Memory<std::uint8_t> tcp_memory_, logged_memory_;
    fc::Memory<const char> password_memory_;
    fc::Memory<const std::uint16_t> port_memory_;
    fc::Memory<const std::uint8_t> host_memory_;
    fc::Memory<const TeamTable> teams_memory_;
    fc::Memory<const std::uint32_t> winner_memory_;
    fc::Memory<LuaState *const> lua_memory_;

    // Views acquired in Prepare; the game module remains loaded while the installed patch uses them.
    char *response_{};
    std::uint8_t *tcp_context_{};
    const char *admin_password_{};
    std::uint8_t *logged_in_{};
    const std::uint16_t *direct_port_{};
    const std::uint8_t *is_host_{};
    const TeamTable *teams_{};
    const std::uint32_t *winning_teams_{};
    LuaState *const *lua_state_{};
};

} // namespace fc::core_plugin::rcon_server::gog
