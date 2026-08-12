#include "rcon.hpp"
#include "../bounded_text.hpp"
#include "layout.hpp"

#include <array>
#include <cstddef>
#include <cstring>
#include <string_view>

namespace fusioncutter::patches::rcon_server {
namespace {

constexpr std::wstring_view kLuaPrefix = L"/lua ";
constexpr std::string_view kInvalidParameters = "invalid parameters\n";
constexpr std::string_view kOkay = "ok\n";
constexpr std::string_view kLuaError = "lua error\n";

// Writes a command result through Aspyr's native RCON response buffer.
void write_response(const ImageContext& image, std::string_view response) noexcept {
    auto* output = image.mutable_at_rva<char[aspyr::layout::kResponseCapacity]>(aspyr::layout::kResponseBufferRva);
    if (output == nullptr) {
        return;
    }
    std::memcpy(*output, response.data(), response.size());
    (*output)[response.size()] = '\0';
}

} // namespace

PatchInstanceSlot<AspyrRcon> AspyrRcon::active_{};
OriginalFunction<AspyrRcon::CommandFunction> AspyrRcon::original_{};

AspyrRcon::AspyrRcon(const TargetContext& target) noexcept : image_(target.image) {}

void AspyrRcon::build_plan(PatchPlan& plan) {
    plan.require_bytes("Validate authenticated RCON call", aspyr::layout::kAuthenticatedCallRva,
                       BytePattern::exact(aspyr::layout::kAuthenticatedCallPreimage));
    plan.require_bytes("Validate Lua wrapper", aspyr::layout::kLuaWrapperRva,
                       BytePattern::exact(aspyr::layout::kLuaWrapperPreimage));
    original_ = plan.inline_hook_with_original("Add RCON Lua command", aspyr::layout::kCommandRva,
                                               BytePattern::exact(aspyr::layout::kCommandPreimage),
                                               static_cast<CommandFunction>(&command_hook));
}

void AspyrRcon::enable_runtime() noexcept {
    active_.publish(*this);
}

void AspyrRcon::disable_runtime() noexcept {
    active_.clear(*this);
}

std::uint64_t AspyrRcon::command_hook(int output, const wchar_t* command, std::uint8_t sender,
                                      std::uint8_t message_type) noexcept {
    if (auto* active = active_.read(); active != nullptr) {
        return active->handle_command(output, command, sender, message_type);
    }
    const auto original = original_.get();
    return original != nullptr ? original(output, command, sender, message_type) : 0;
}

std::uint64_t AspyrRcon::handle_command(int output, const wchar_t* command, std::uint8_t sender,
                                        std::uint8_t message_type) noexcept {
    const auto original = original_.get();
    const auto pass_through = [&]() noexcept {
        return original != nullptr ? original(output, command, sender, message_type) : 0;
    };

    const auto* details = image_.read_at_rva<std::uint8_t>(aspyr::layout::kCommandDetailsRva);
    const auto* logged_in = image_.read_at_rva<std::uint8_t>(aspyr::layout::kLoggedInRva);
    if (command == nullptr || details == nullptr || logged_in == nullptr) {
        return pass_through();
    }

    const auto input = bounded_string_view(command, aspyr::layout::kCommandCapacity);
    // Claim only authenticated TCP RCON /lua commands; every other native command passes through.
    const auto handles_lua = output == -1 && sender == 0 && message_type == 0 && *details == 1 && *logged_in == 1 &&
                             input.starts_with(kLuaPrefix);
    if (!handles_lua) {
        return pass_through();
    }
    const auto body = input.substr(kLuaPrefix.size());
    if (body.empty() || input.size() == aspyr::layout::kCommandCapacity) {
        write_response(image_, kInvalidParameters);
        return 1;
    }

    // The native Lua wrapper consumes narrow source bytes. Reject ambiguous locale conversion from the wide command.
    std::array<char, aspyr::layout::kCommandCapacity> code{};
    for (std::size_t index = 0; index < body.size(); ++index) {
        if (body[index] > 0x7F) {
            write_response(image_, kInvalidParameters);
            return 1;
        }
        code[index] = static_cast<char>(body[index]);
    }

    using LuaFunction = int (*)(void*, const char*, std::size_t, const char*);
    const auto* state = image_.read_at_rva<void*>(aspyr::layout::kLuaStateRva);
    const auto lua = image_.function_at_rva<LuaFunction>(aspyr::layout::kLuaWrapperRva);
    const auto result = state != nullptr && *state != nullptr ? lua(*state, code.data(), body.size(), "=rcon") : -1;
    write_response(image_, result == 0 ? kOkay : kLuaError);
    return 1;
}

} // namespace fusioncutter::patches::rcon_server
