#pragma once

#include <FusionCutter/outcome.hpp>

#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <expected>
#include <filesystem>
#include <memory>
#include <mutex>
#include <stop_token>
#include <thread>

namespace fusioncutter::patches::screenshot_fix {

struct ScreenshotFrame {
    std::uint32_t width{};
    std::uint32_t height{};
    std::unique_ptr<std::byte[]> bgra_pixels;
};

// Writes captured frames as sequential 24-bit TGA files in the game's ScreenShots folder.
class ScreenshotWriter {
  public:
    ScreenshotWriter() = default;
    ScreenshotWriter(const ScreenshotWriter&) = delete;
    ScreenshotWriter& operator=(const ScreenshotWriter&) = delete;
    ~ScreenshotWriter();

    // Resolves the game-local output folder and starts the background writer.
    [[nodiscard]] std::expected<void, OutcomeReason> prepare(std::uintptr_t game_module);
    // Queues one capture without waiting; a second capture is rejected until the first has been claimed.
    [[nodiscard]] bool submit(std::unique_ptr<ScreenshotFrame> frame) noexcept;

  private:
    // Waits for captured frames so file-system work stays off the game thread.
    void worker_main(std::stop_token stop_token) noexcept;
    // Encodes one frame as an uncompressed 24-bit TGA.
    void write_frame(const ScreenshotFrame& frame);
    // Chooses the first unused screenshot_NNNN.tga name without overwriting an existing capture.
    [[nodiscard]] std::filesystem::path next_path() const;

    std::filesystem::path output_directory_;
    std::mutex queue_mutex_;
    std::condition_variable_any ready_;
    std::unique_ptr<ScreenshotFrame> pending_;
    std::jthread worker_;
};

} // namespace fusioncutter::patches::screenshot_fix
