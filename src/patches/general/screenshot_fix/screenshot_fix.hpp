#pragma once

#include "screenshot_writer.hpp"

#include <FusionCutter/patch.hpp>

#include <cstdint>

struct IDirect3DDevice9;

namespace fusioncutter::patches::screenshot_fix {

// Redirects retail Print Screen requests away from the crashing native routine to a Direct3D backbuffer capture.
class ScreenshotFix final : public RuntimePatch {
  public:
    explicit ScreenshotFix(const TargetContext& target) noexcept;
    ~ScreenshotFix() override;

    // Verifies and redirects the retail call site that invokes Screenshot::RequestScreenshot.
    void build_plan(PatchPlan& plan) override;
    // Starts the background writer before the replacement can receive Print Screen requests.
    [[nodiscard]] std::expected<void, OutcomeReason> prepare_runtime() override;
    void enable_runtime() noexcept override;
    void disable_runtime() noexcept override;

  private:
    // Captures the current backbuffer on the game thread and queues it for background file output.
    void capture_screenshot() noexcept;
    // Copies the Direct3D backbuffer into tightly packed, CPU-owned BGRA pixels.
    [[nodiscard]] std::unique_ptr<ScreenshotFrame> capture_frame(IDirect3DDevice9& device) noexcept;

    // Receives the redirected Print Screen request through the active patch instance.
    static void __cdecl request_screenshot() noexcept;

    ImageContext image_;
    IDirect3DDevice9* const* device_slot_{};
    TargetLayout target_{};
    ScreenshotWriter writer_;

    inline static PatchInstanceSlot<ScreenshotFix> active_;
};

} // namespace fusioncutter::patches::screenshot_fix
