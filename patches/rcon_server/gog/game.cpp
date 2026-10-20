#include "game.hpp"

#include "../bounded_text.hpp"
#include "../native_result.hpp"
#include "layout.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>

namespace fc::core_plugin::rcon_server::gog {
namespace {

constexpr std::string_view kLuaPrefix = "/lua ";
constexpr std::string_view kInvalidParameters = "invalid parameters\n";
constexpr std::string_view kOkay = "ok\n";
constexpr std::string_view kLuaError = "lua error\n";
constexpr std::string_view kBusy = "busy\n";
constexpr std::string_view kProtocolLimitError = "RCON response exceeds protocol limit";
} // namespace

void GameBridge::plan(fc::Plan &plan, const Layout &layout) {
    command_parser_ = plan.function(layout.command_parser);

    // The observation follows completed formatting, before the caller removes its variadic stack arguments.
    plan.check(layout.chat_setup, layout.chat_setup_size);
    plan.check(layout.chat_format, layout.chat_format_size);
    plan.check(layout.chat_formatter, layout.chat_formatter_size);
    plan.observe_instruction(layout.chat_result, fc::before([this](const fc::CpuContext &context) noexcept {
                                 capture_chat(context);
                             }));

    lua_load_ = plan.function(layout.lua_load);
    lua_call_ = plan.function(layout.lua_call);
    lua_get_top_ = plan.function(layout.lua_get_top);
    lua_set_top_ = plan.function(layout.lua_set_top);

    response_memory_ = plan.access(layout.response);
    tcp_memory_ = plan.access(layout.tcp_context);
    logged_memory_ = plan.access(layout.logged_in);
    password_memory_ = plan.access(layout.admin_password);
    port_memory_ = plan.access(layout.direct_port);
    host_memory_ = plan.access(layout.is_host);
    teams_memory_ = plan.access(layout.teams);
    winner_memory_ = plan.access(layout.winning_teams);
    lua_memory_ = plan.access(layout.lua_state);
}

namespace {

// Retains the planned memory view's pointer; its game image must remain loaded while the pointer is used.
template <class T> fc::Result<> retain_view(const fc::Memory<T> &memory, T *&output) {
    auto view = memory.view();
    if (!view) {
        return std::unexpected(view.error());
    }

    output = view->data();
    return {};
}

} // namespace

fc::Result<> GameBridge::prepare() {
    // Resolve the prepared memory handles into direct views used by command execution and listener status.
    if (auto value = retain_view(response_memory_, response_); !value) {
        return value;
    }
    if (auto value = retain_view(tcp_memory_, tcp_context_); !value) {
        return value;
    }
    if (auto value = retain_view(logged_memory_, logged_in_); !value) {
        return value;
    }
    if (auto value = retain_view(password_memory_, admin_password_); !value) {
        return value;
    }
    if (auto value = retain_view(port_memory_, direct_port_); !value) {
        return value;
    }
    if (auto value = retain_view(host_memory_, is_host_); !value) {
        return value;
    }
    if (auto value = retain_view(teams_memory_, teams_); !value) {
        return value;
    }
    if (auto value = retain_view(winner_memory_, winning_teams_); !value) {
        return value;
    }
    return retain_view(lua_memory_, lua_state_);
}

std::uint16_t GameBridge::direct_port() const noexcept {
    return *direct_port_;
}

std::string_view GameBridge::admin_password() const noexcept {
    return bounded_text(admin_password_, Layout::kPasswordCapacity);
}

bool GameBridge::has_winner() const noexcept {
    return *winning_teams_ != Layout::kNoWinner;
}

std::string GameBridge::execute(std::string_view command) {
    if (!server_ready(command)) {
        return std::string{kBusy};
    }

    // Handle the legacy administration Lua command locally; send other commands to NetAdmin.
    if (command.starts_with(kLuaPrefix)) {
        const auto code = command.substr(kLuaPrefix.size());
        if (code.empty()) {
            return std::string{kInvalidParameters};
        }
        return std::string{execute_lua(code) == 0 ? kOkay : kLuaError};
    }
    return execute_native(command);
}

void GameBridge::enqueue_chat(std::string_view message) noexcept {
    // Drop this message on lock contention so the game callback never waits for the service worker.
    if (chat_lock_.test_and_set(std::memory_order_acquire)) {
        dropped_chat_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    if (chat_count_ == chat_queue_.size()) {
        // Preserve the newest notification by discarding the oldest entry from the fixed queue.
        chat_head_ = (chat_head_ + 1) % chat_queue_.size();
        --chat_count_;
        dropped_chat_.fetch_add(1, std::memory_order_relaxed);
    }

    // Copy out of the formatter's temporary buffer, reserving one byte for the terminator when truncating.
    auto &entry = chat_queue_[(chat_head_ + chat_count_) % chat_queue_.size()];
    entry.size = std::min(message.size(), entry.text.size() - 1);
    std::memcpy(entry.text.data(), message.data(), entry.size);
    entry.text[entry.size] = '\0';
    ++chat_count_;
    chat_lock_.clear(std::memory_order_release);
}

GameBridge::ChatDrainResult GameBridge::drain_chat(std::span<ChatMessage> output) noexcept {
    if (chat_lock_.test_and_set(std::memory_order_acquire)) {
        return {};
    }

    // Drain in arrival order, handling wraparound before advancing the queue head.
    const auto count = std::min(chat_count_, output.size());
    for (std::size_t index = 0; index < count; ++index) {
        output[index] = chat_queue_[(chat_head_ + index) % chat_queue_.size()];
    }

    chat_head_ = (chat_head_ + count) % chat_queue_.size();
    chat_count_ -= count;
    const auto queue_empty = chat_count_ == 0;
    chat_lock_.clear(std::memory_order_release);
    return {.count = count, .queue_empty = queue_empty};
}

std::uint64_t GameBridge::dropped_chat_messages() const noexcept {
    return dropped_chat_.load(std::memory_order_relaxed);
}

bool GameBridge::server_ready(std::string_view command) const noexcept {
    return *is_host_ == 1 && !has_winner() && (command != "/status" || status_ready());
}

bool GameBridge::status_ready() const noexcept {
    // NetAdmin's /status handler dereferences team slots 1 and 2 before a mission may have populated them.
    if (*teams_ == nullptr) {
        return false;
    }
    const auto teams = *teams_;
    return teams[1] != nullptr && teams[2] != nullptr;
}

std::string GameBridge::execute_native(std::string_view command) {
    // NetAdmin expects a wide command encoded from the process's ANSI code page.
    std::array<wchar_t, Layout::kCommandCapacity> wide_command{};
    const auto converted = MultiByteToWideChar(CP_ACP, 0, command.data(), static_cast<int>(command.size()), wide_command.data(),
                                               static_cast<int>(wide_command.size() - 1));
    if (converted <= 0) {
        return std::string{kInvalidParameters};
    }

    // The game's TCP handler sets both flags only while parsing an authenticated command from its socket.
    const auto previous_logged_in = std::exchange(*logged_in_, std::uint8_t{1});
    const auto previous_tcp_context = std::exchange(*tcp_context_, std::uint8_t{1});
    native_result(command_parser_.call(-1, wide_command.data(), false, false));
    *tcp_context_ = previous_tcp_context;
    *logged_in_ = previous_logged_in;

    // Copy the game's shared response before another command replaces it; a missing terminator uses the protocol error.
    const auto response = bounded_text(response_, Layout::kResponseCapacity);
    return response.size() == Layout::kResponseCapacity ? std::string{kProtocolLimitError} : std::string{response};
}

int GameBridge::execute_lua(std::string_view code) noexcept {
    if (*lua_state_ == nullptr) {
        return -1;
    }

    const auto state = *lua_state_;
    // The Lua helpers retain results and errors, so restore the public stack height after every command.
    const auto original_top = native_result(lua_get_top_.call(state));

    // Execute only a successfully loaded chunk; both Lua failure paths continue to stack restoration.
    auto result = native_result(lua_load_.call(state, code.data(), code.size(), "=rcon"));
    if (result == 0) {
        result = native_result(lua_call_.call(state, 0, 0, 0));
    }

    native_result(lua_set_top_.call(state, original_top));
    return result;
}

void GameBridge::capture_chat(const fc::CpuContext &context) noexcept {
    // The observed x86 cleanup instruction still has the formatter's buffer and capacity at the top of the stack.
    const auto stack = fc::stack_pointer(context);
    const std::uintptr_t buffer_address = fc::read_field<std::uint32_t>(stack);
    const auto *buffer = reinterpret_cast<const char *>(buffer_address);
    const auto capacity =
        std::min(static_cast<std::size_t>(fc::read_field<std::uint32_t>(stack, sizeof(std::uint32_t))), kChatMessageCapacity);
    if (buffer != nullptr && capacity != 0) {
        enqueue_chat(bounded_text(buffer, capacity));
    }
}

} // namespace fc::core_plugin::rcon_server::gog
