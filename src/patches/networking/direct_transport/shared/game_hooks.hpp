#pragma once

#include "game_layout.hpp"
#include "game_transport.hpp"

#include <FusionCutter/patch.hpp>

namespace fusioncutter::patches::direct_transport {

// Installs the game's x86 packet and lobby callbacks, then publishes the active transport to them.
class GameHooks {
  public:
    GameHooks(ImageContext image, const GameLayout& layout) noexcept;

    // Validates native helpers and installs the shared packet and lobby callback hooks.
    void build_plan(PatchPlan& plan);
    // Publishes or clears the role-specific transport used by the installed callbacks.
    void enable(GameTransport& transport) noexcept;
    void disable(GameTransport& transport) noexcept;

  private:
    ImageContext image_;
    const GameLayout& layout_;
};

// Re-enters the preserved native disconnect path for a policy-driven removal.
void disconnect_native(int physical_primary) noexcept;
// Delivers a reconstructed direct packet through the preserved native intake path.
void submit_direct_native(void* packet, void* endpoint) noexcept;

#if defined(FC_DIRECT_TRANSPORT_ABI_TEST)
enum class GameHookPoint {
    FinalSend,
    GroupSend,
};

struct GameHookOriginals {
    void* final_send;
    void* group_send;
};

void configure_game_hooks_for_test(GameTransport& transport, const GameHookOriginals& originals) noexcept;
[[nodiscard]] void* game_hook_for_test(GameHookPoint point) noexcept;
#endif

} // namespace fusioncutter::patches::direct_transport
