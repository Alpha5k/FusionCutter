#include "game.hpp"
#include "../bounded_text.hpp"
#include "command.hpp"
#include "layout.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>

namespace fusioncutter::patches::rcon_server::gog {
namespace {

constexpr std::string_view kLuaPrefix = "/lua ";
constexpr std::string_view kInvalidParameters = "invalid parameters\n";
constexpr std::string_view kOkay = "ok\n";
constexpr std::string_view kLuaError = "lua error\n";
constexpr std::string_view kBusy = "busy\n";
constexpr std::string_view kProtocolLimitError = "RCON response exceeds protocol limit";

} // namespace

PatchInstanceSlot<Game> Game::active_{};
Game::Formatter Game::formatter_ = &Game::capture_chat;

Game::Game(ImageContext image) noexcept : image_(image) {}

Game::~Game() {
    disable();
}

void Game::build_plan(PatchPlan& plan) {
    plan.require_bytes("Validate native RCON command", layout::kCommandRva,
                       BytePattern::exact(layout::kCommandPreimage));
    plan.require_bytes("Validate chat capture call", layout::kChatCallRva,
                       BytePattern::exact(layout::kChatCallPreimage));
    plan.require_bytes("Validate Lua load buffer", layout::kLuaLoadBufferRva,
                       BytePattern::exact(layout::kLuaLoadBufferPreimage));
    plan.require_bytes("Validate Lua protected call", layout::kLuaPcallRva,
                       BytePattern::exact(layout::kLuaPcallPreimage));
    plan.require_bytes("Validate Lua stack restore", layout::kLuaSetTopRva,
                       BytePattern::exact(layout::kLuaSetTopPreimage));

    // Preserve the native FF 15 indirect call and replace only its IAT operand with our formatter slot.
    const auto expected_import = static_cast<std::uint32_t>(image_.address_at_rva(layout::kSnprintfImportRva));
    plan.checked_write("Capture RCON chat", layout::kChatOperandRva, expected_import,
                       PatchAddress::absolute(&formatter_));
}

std::expected<void, OutcomeReason> Game::validate() const {
    const auto in_bounds = image_.contains_rva(layout::kResponseBufferRva, layout::kResponseCapacity) &&
                           image_.contains_rva(layout::kCommandDetailsRva, sizeof(std::uint8_t)) &&
                           image_.contains_rva(layout::kAdminPasswordRva, layout::kAdminPasswordCapacity) &&
                           image_.contains_rva(layout::kLoggedInRva, sizeof(std::uint8_t)) &&
                           image_.contains_rva(layout::kGamePortRva, sizeof(std::uint16_t)) &&
                           image_.contains_rva(layout::kIdleRva, sizeof(std::uint8_t)) &&
                           image_.contains_rva(layout::kTeamArrayRva, sizeof(std::uint32_t)) &&
                           image_.contains_rva(layout::kMapStatusRva, sizeof(std::uint8_t)) &&
                           image_.contains_rva(layout::kLuaStateRva, sizeof(std::uint32_t));
    if (!in_bounds) {
        return std::unexpected(
            OutcomeReason{"RCON native data is outside the recognized image", "Validate RCON native data", {}});
    }
    return {};
}

void Game::enable() noexcept {
    active_.publish(*this);
}

void Game::disable() noexcept {
    active_.clear(*this);
}

std::uint16_t Game::game_port() const noexcept {
    return *image_.read_at_rva<std::uint16_t>(layout::kGamePortRva);
}

std::string_view Game::admin_password() const noexcept {
    const auto* password = image_.read_at_rva<char[layout::kAdminPasswordCapacity]>(layout::kAdminPasswordRva);
    return bounded_string_view(*password, layout::kAdminPasswordCapacity);
}

bool Game::map_is_idle() const noexcept {
    return *image_.read_at_rva<std::uint8_t>(layout::kMapStatusRva) == layout::kMapIdle;
}

std::string Game::execute(std::string_view command) {
    if (!server_ready(command)) {
        return std::string{kBusy};
    }
    if (command.starts_with(kLuaPrefix)) {
        const auto code = command.substr(kLuaPrefix.size());
        if (code.empty()) {
            return std::string{kInvalidParameters};
        }
        return std::string{execute_lua(code) == 0 ? kOkay : kLuaError};
    }
    return execute_native(command);
}

Game::ChatDrainResult Game::drain_chat(std::span<ChatMessage> output) noexcept {
    if (chat_lock_.test_and_set(std::memory_order_acquire)) {
        return {};
    }

    const auto count = std::min(chat_count_, output.size());
    for (std::size_t index = 0; index < count; ++index) {
        output[index] = chat_queue_[(chat_head_ + index) % chat_queue_.size()];
    }
    chat_head_ = (chat_head_ + count) % chat_queue_.size();
    chat_count_ -= count;
    const auto empty = chat_count_ == 0;
    chat_lock_.clear(std::memory_order_release);
    return {count, empty};
}

bool Game::server_ready(std::string_view command) const noexcept {
    const auto* idle = image_.read_at_rva<std::uint8_t>(layout::kIdleRva);
    return *idle == 1 && map_is_idle() && (command != "/status" || status_ready());
}

bool Game::status_ready() const noexcept {
    // The native /status handler reads both team entries; invoking it before this table is populated is unsafe.
    const auto* team_array = image_.read_at_rva<std::uint32_t>(layout::kTeamArrayRva);
    if (*team_array == 0) {
        return false;
    }
    const auto* teams = reinterpret_cast<const std::uint32_t*>(*team_array);
    return teams[1] != 0 && teams[2] != 0;
}

std::string Game::execute_native(std::string_view command) {
    std::array<wchar_t, layout::kCommandCapacity> wide_command{};
    const auto converted = MultiByteToWideChar(CP_ACP, 0, command.data(), static_cast<int>(command.size()),
                                               wide_command.data(), static_cast<int>(wide_command.size() - 1));
    if (converted <= 0) {
        return std::string{kInvalidParameters};
    }

    // Commands arriving through our listener bypass the game's native authentication path. Set its two context bytes
    // only for the synchronous native call, then restore the game-owned values.
    auto* logged_in = image_.mutable_at_rva<std::uint8_t>(layout::kLoggedInRva);
    auto* details = image_.mutable_at_rva<std::uint8_t>(layout::kCommandDetailsRva);
    const auto previous_logged_in = std::exchange(*logged_in, std::uint8_t{1});
    const auto previous_details = std::exchange(*details, std::uint8_t{1});
    static_cast<void>(
        execute_native_command(image_.address_at_rva(layout::kCommandRva), -1, wide_command.data(), 0, 0));
    *details = previous_details;
    *logged_in = previous_logged_in;

    const auto* response = image_.read_at_rva<char[layout::kResponseCapacity]>(layout::kResponseBufferRva);
    const auto text = bounded_string_view(*response, layout::kResponseCapacity);
    return text.size() == layout::kResponseCapacity ? std::string{kProtocolLimitError} : std::string{text};
}

int Game::execute_lua(std::string_view code) noexcept {
    const auto* state_slot = image_.read_at_rva<std::uint32_t>(layout::kLuaStateRva);
    const auto state = *state_slot;
    if (state == 0) {
        return -1;
    }

    // The Lua helpers leave results/errors on the stack. Preserve its original height across every RCON command.
    const auto top = *reinterpret_cast<const std::uint32_t*>(state + 0x08);
    const auto base = *reinterpret_cast<const std::uint32_t*>(state + 0x0C);
    const auto original_top = static_cast<int>((top - base) / 8);
    const auto load_buffer = image_.function_at_rva<LuaLoadBufferFunction>(layout::kLuaLoadBufferRva);
    const auto protected_call = image_.function_at_rva<LuaPcallFunction>(layout::kLuaPcallRva);
    const auto set_top = image_.function_at_rva<LuaSetTopFunction>(layout::kLuaSetTopRva);

    auto result = load_buffer(state, code.data(), code.size(), "=rcon");
    if (result == 0) {
        result = protected_call(state, 0, 0, 0);
    }
    set_top(state, original_top);
    return result;
}

void Game::enqueue_chat(std::string_view message) noexcept {
    // This runs in the game's formatting call. Never wait for the service worker: drop on contention and retain only
    // the newest bounded messages when the ring is full.
    if (chat_lock_.test_and_set(std::memory_order_acquire)) {
        return;
    }
    if (chat_count_ == chat_queue_.size()) {
        chat_head_ = (chat_head_ + 1) % chat_queue_.size();
        --chat_count_;
    }
    auto& entry = chat_queue_[(chat_head_ + chat_count_) % chat_queue_.size()];
    entry.size = std::min(message.size(), entry.text.size() - 1);
    std::memcpy(entry.text.data(), message.data(), entry.size);
    entry.text[entry.size] = '\0';
    ++chat_count_;
    chat_lock_.clear(std::memory_order_release);
}

int __cdecl Game::capture_chat(char* buffer, std::size_t size, const char* format, ...) noexcept {
    va_list arguments;
    va_start(arguments, format);
    const auto result = std::vsnprintf(buffer, size, format, arguments);
    va_end(arguments);

    if (buffer != nullptr && size != 0) {
        buffer[size - 1] = '\0';
        if (auto* active = active_.read(); active != nullptr) {
            active->enqueue_chat(bounded_string_view(buffer, size));
        }
    }
    return result;
}

} // namespace fusioncutter::patches::rcon_server::gog
