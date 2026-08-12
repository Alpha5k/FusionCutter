#include "screenshot_fix.hpp"

#include "layout.hpp"

#include <FusionCutter/reporting.hpp>

#include <d3d9.h>
#include <wrl/client.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <limits>
#include <memory>
#include <new>
#include <string_view>
#include <utility>

namespace fusioncutter::patches::screenshot_fix {
namespace {

using Microsoft::WRL::ComPtr;

constexpr PatchId kPatchId = "ScreenshotFix";
constexpr std::size_t kBytesPerPixel = 4;
constexpr std::size_t kMaximumCaptureBytes = 128U * 1024U * 1024U;
constexpr std::uint32_t kMaximumTgaDimension = (std::numeric_limits<std::uint16_t>::max)();

void log_capture_failure(std::string_view operation, HRESULT result) noexcept {
    try {
        logging::warning(
            kPatchId,
            std::format("Direct3D screenshot capture failed with result 0x{:08X}", static_cast<std::uint32_t>(result)),
            operation);
    } catch (...) {
        logging::warning(kPatchId, "Direct3D screenshot capture failed", operation);
    }
}

// Bound the temporary pixel copy and the 16-bit dimensions stored by the TGA format.
[[nodiscard]] std::size_t capture_size(const D3DSURFACE_DESC& surface) noexcept {
    if (surface.Width == 0 || surface.Height == 0 || surface.Width > kMaximumTgaDimension ||
        surface.Height > kMaximumTgaDimension) {
        return 0;
    }

    const auto row_size = static_cast<std::size_t>(surface.Width) * kBytesPerPixel;
    if (surface.Height > kMaximumCaptureBytes / row_size) {
        return 0;
    }
    return row_size * surface.Height;
}

} // namespace

ScreenshotFix::ScreenshotFix(const TargetContext& target) noexcept
    : game_module_(target.image.base),
      device_slot_(target.image.read_at_rva<IDirect3DDevice9*>(layout_for(target.layout).device_slot_rva)),
      target_(target.layout) {}

ScreenshotFix::~ScreenshotFix() {
    disable_runtime();
}

void ScreenshotFix::build_plan(PatchPlan& plan) {
    const auto& layout = layout_for(target_);
    plan.require_bytes("Verify Direct3D device global", layout.device_reference_rva,
                       BytePattern::exact(layout.device_reference));
    plan.redirect_call("Replace broken screenshot capture", layout.request_call_rva,
                       BytePattern::exact(layout.request_call), &ScreenshotFix::request_screenshot);
}

std::expected<void, OutcomeReason> ScreenshotFix::prepare_runtime() {
    if (device_slot_ == nullptr) {
        return std::unexpected(
            OutcomeReason{"The Direct3D device slot is outside the recognized game image", "Prepare screenshots", {}});
    }
    return writer_.prepare(game_module_);
}

void ScreenshotFix::enable_runtime() noexcept {
    active_.publish(*this);
}

void ScreenshotFix::disable_runtime() noexcept {
    active_.clear(*this);
}

void ScreenshotFix::capture_screenshot() noexcept {
    auto* device = *device_slot_;
    if (device == nullptr) {
        logging::warning(kPatchId, "A screenshot was requested before the Direct3D device was available",
                         "Capture screenshot");
        return;
    }

    auto frame = capture_frame(*device);
    if (frame != nullptr && !writer_.submit(std::move(frame))) {
        logging::warning(kPatchId, "A screenshot request was dropped while the previous capture was being saved",
                         "Queue screenshot file");
    }
}

std::unique_ptr<ScreenshotFrame> ScreenshotFix::capture_frame(IDirect3DDevice9& device) noexcept {
    ComPtr<IDirect3DSurface9> backbuffer;
    if (const auto result = device.GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backbuffer); FAILED(result)) {
        log_capture_failure("Read screenshot backbuffer", result);
        return {};
    }

    D3DSURFACE_DESC description{};
    if (const auto result = backbuffer->GetDesc(&description); FAILED(result)) {
        log_capture_failure("Read screenshot dimensions", result);
        return {};
    }

    const auto pixel_bytes = capture_size(description);
    if (pixel_bytes == 0) {
        logging::warning(kPatchId, "The screenshot dimensions exceed the supported capture size",
                         "Validate screenshot dimensions");
        return {};
    }

    ComPtr<IDirect3DSurface9> render_target;
    if (const auto result = device.CreateRenderTarget(description.Width, description.Height, D3DFMT_A8R8G8B8,
                                                      D3DMULTISAMPLE_NONE, 0, FALSE, &render_target, nullptr);
        FAILED(result)) {
        log_capture_failure("Create screenshot render target", result);
        return {};
    }
    if (const auto result = device.StretchRect(backbuffer.Get(), nullptr, render_target.Get(), nullptr, D3DTEXF_NONE);
        FAILED(result)) {
        log_capture_failure("Copy screenshot backbuffer", result);
        return {};
    }

    ComPtr<IDirect3DSurface9> system_memory;
    if (const auto result = device.CreateOffscreenPlainSurface(description.Width, description.Height, D3DFMT_A8R8G8B8,
                                                               D3DPOOL_SYSTEMMEM, &system_memory, nullptr);
        FAILED(result)) {
        log_capture_failure("Create screenshot readback surface", result);
        return {};
    }
    if (const auto result = device.GetRenderTargetData(render_target.Get(), system_memory.Get()); FAILED(result)) {
        log_capture_failure("Read screenshot pixels", result);
        return {};
    }

    D3DLOCKED_RECT locked{};
    if (const auto result = system_memory->LockRect(&locked, nullptr, D3DLOCK_READONLY); FAILED(result)) {
        log_capture_failure("Lock screenshot pixels", result);
        return {};
    }

    auto frame = std::unique_ptr<ScreenshotFrame>{new (std::nothrow) ScreenshotFrame};
    if (frame != nullptr) {
        frame->width = description.Width;
        frame->height = description.Height;
        frame->bgra_pixels.reset(new (std::nothrow) std::byte[pixel_bytes]);
    }

    const auto row_size = static_cast<std::size_t>(description.Width) * kBytesPerPixel;
    if (frame != nullptr && frame->bgra_pixels != nullptr && locked.pBits != nullptr && locked.Pitch >= 0 &&
        static_cast<std::size_t>(locked.Pitch) >= row_size) {
        const auto* source = static_cast<const std::byte*>(locked.pBits);
        for (std::uint32_t row = 0; row < description.Height; ++row) {
            std::memcpy(frame->bgra_pixels.get() + static_cast<std::size_t>(row) * row_size,
                        source + static_cast<std::size_t>(row) * locked.Pitch, row_size);
        }
    } else {
        frame.reset();
    }
    system_memory->UnlockRect();

    if (frame == nullptr) {
        logging::warning(kPatchId, "Screenshot pixel storage could not be prepared", "Copy screenshot pixels");
    }
    return frame;
}

void __cdecl ScreenshotFix::request_screenshot() noexcept {
    if (auto* patch = active_.read(); patch != nullptr) {
        patch->capture_screenshot();
    }
}

} // namespace fusioncutter::patches::screenshot_fix
