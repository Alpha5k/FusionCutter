#pragma once

#include <FusionCutter/patch.hpp>

#include <cstdint>

namespace fusioncutter::patches::rcon_server {

// Adds /lua to Aspyr's native authenticated RCON command path.
class AspyrRcon final : public RuntimePatch {
  public:
    explicit AspyrRcon(const TargetContext& target) noexcept;

    // Validates the authenticated path and hooks Aspyr's native RCON dispatcher.
    void build_plan(PatchPlan& plan) override;
    void enable_runtime() noexcept override;
    void disable_runtime() noexcept override;

  private:
    using CommandFunction = std::uint64_t (*)(int, const wchar_t*, std::uint8_t, std::uint8_t);

    [[nodiscard]] static std::uint64_t command_hook(int output, const wchar_t* command, std::uint8_t sender,
                                                    std::uint8_t message_type) noexcept;

    // Handles authenticated /lua commands and passes every other command to native RCON.
    [[nodiscard]] std::uint64_t handle_command(int output, const wchar_t* command, std::uint8_t sender,
                                               std::uint8_t message_type) noexcept;

    static PatchInstanceSlot<AspyrRcon> active_;
    static OriginalFunction<CommandFunction> original_;

    ImageContext image_;
};

} // namespace fusioncutter::patches::rcon_server
