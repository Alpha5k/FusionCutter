#pragma once

#include "game_layout.hpp"
#include "game_transport.hpp"

#include <FusionCutter/patch.hpp>

namespace fusioncutter::patches::direct_transport {

// Installs Direct Transport's native helper requirements and Galaxy lobby callbacks.
class LobbyHooks {
  public:
    LobbyHooks(ImageContext image, const GameLayout& layout) noexcept;

    void build_plan(PatchPlan& plan);
    void enable(GameTransport& transport) noexcept;
    void disable(GameTransport& transport) noexcept;

  private:
    ImageContext image_;
    const GameLayout& layout_;
};

} // namespace fusioncutter::patches::direct_transport
