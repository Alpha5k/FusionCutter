#pragma once

#include <FusionCutter/patch.hpp>

#include <cstddef>
#include <cstdint>

namespace fusioncutter::patches::galaxypeer_observer {

// Captures the public IPv4 address Galaxy discovers for the server's RakNet peer.
class GalaxyPeerObserver final : public RuntimePatch {
  public:
    struct Layout {
        std::uint32_t get_external_id_rva;
        std::uint32_t raw_vtable_rva;
    };

    explicit GalaxyPeerObserver(const TargetContext& target) noexcept;
    ~GalaxyPeerObserver() override;

    // Hooks Galaxy's external-address query only when Direct Transport requested the observation.
    void build_plan(PatchPlan& plan) override;
    void enable_runtime() noexcept override;
    void disable_runtime() noexcept override;

    // Validates and publishes a public IPv4 result from the server's active peer.
    void observe(void* raw_peer, const void* result) noexcept;

  private:
    ImageContext image_;
    Layout layout_{};
    bool requested_{};
};

} // namespace fusioncutter::patches::galaxypeer_observer
