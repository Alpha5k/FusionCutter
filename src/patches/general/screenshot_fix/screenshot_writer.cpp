#include "screenshot_writer.hpp"

#include <FusionCutter/reporting.hpp>

#include <Windows.h>

#include <algorithm>
#include <array>
#include <exception>
#include <format>
#include <fstream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace fusioncutter::patches::screenshot_fix {
namespace {

constexpr PatchId kPatchId = "ScreenshotFix";
constexpr std::size_t kMaximumPathCharacters = 32'768;
constexpr std::uint32_t kMaximumScreenshotNumber = 9'999;
constexpr std::size_t kBgraBytesPerPixel = 4;
constexpr std::size_t kTgaBytesPerPixel = 3;

[[nodiscard]] OutcomeReason writer_error(std::string message, std::string operation) {
    return {std::move(message), std::move(operation), {}};
}

// Anchor screenshot output to the game installation rather than the process working directory.
[[nodiscard]] std::expected<std::filesystem::path, OutcomeReason> module_directory(std::uintptr_t module) {
    std::wstring path(kMaximumPathCharacters, L'\0');
    const auto length =
        GetModuleFileNameW(reinterpret_cast<HMODULE>(module), path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length == path.size()) {
        return std::unexpected(
            writer_error("The game directory could not be resolved for screenshot output", "Prepare screenshots"));
    }

    path.resize(length);
    return std::filesystem::path{std::move(path)}.parent_path();
}

// TGA stores this fixed 18-byte header before bottom-up BGR pixel rows.
[[nodiscard]] std::array<std::byte, 18> tga_header(std::uint32_t width, std::uint32_t height) noexcept {
    std::array<std::byte, 18> header{};
    header[2] = std::byte{2};
    header[12] = std::byte{static_cast<std::uint8_t>(width)};
    header[13] = std::byte{static_cast<std::uint8_t>(width >> 8)};
    header[14] = std::byte{static_cast<std::uint8_t>(height)};
    header[15] = std::byte{static_cast<std::uint8_t>(height >> 8)};
    header[16] = std::byte{24};
    return header;
}

} // namespace

ScreenshotWriter::~ScreenshotWriter() {
    if (worker_.joinable()) {
        worker_.request_stop();
        ready_.notify_all();
        worker_.join();
    }
}

std::expected<void, OutcomeReason> ScreenshotWriter::prepare(std::uintptr_t game_module) {
    try {
        auto game_directory = module_directory(game_module);
        if (!game_directory.has_value()) {
            return std::unexpected(std::move(game_directory.error()));
        }

        output_directory_ = *game_directory / L"ScreenShots";
        worker_ = std::jthread([this](std::stop_token stop_token) noexcept {
            worker_main(stop_token);
        });
        return {};
    } catch (const std::exception& error) {
        return std::unexpected(
            writer_error("Screenshot writer preparation failed: " + std::string(error.what()), "Prepare screenshots"));
    }
}

bool ScreenshotWriter::submit(std::unique_ptr<ScreenshotFrame> frame) noexcept {
    std::unique_lock lock(queue_mutex_, std::try_to_lock);
    if (!lock.owns_lock() || pending_ != nullptr) {
        return false;
    }

    pending_ = std::move(frame);
    lock.unlock();
    ready_.notify_one();
    return true;
}

void ScreenshotWriter::worker_main(std::stop_token stop_token) noexcept {
    while (!stop_token.stop_requested()) {
        std::unique_lock lock(queue_mutex_);
        if (!ready_.wait(lock, stop_token, [this] {
                return pending_ != nullptr;
            })) {
            return;
        }
        auto frame = std::move(pending_);
        lock.unlock();

        try {
            write_frame(*frame);
        } catch (const std::exception& error) {
            logging::error(kPatchId, "Screenshot write failed: " + std::string(error.what()), "Write screenshot file");
        } catch (...) {
            logging::error(kPatchId, "Screenshot write failed unexpectedly", "Write screenshot file");
        }
    }
}

void ScreenshotWriter::write_frame(const ScreenshotFrame& frame) {
    std::error_code directory_error;
    std::filesystem::create_directories(output_directory_, directory_error);
    if (directory_error) {
        throw std::system_error(directory_error, "could not create the ScreenShots folder");
    }

    const auto path = next_path();
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        throw std::runtime_error("could not create " + path.filename().string());
    }

    const auto header = tga_header(frame.width, frame.height);
    file.write(reinterpret_cast<const char*>(header.data()), static_cast<std::streamsize>(header.size()));

    std::vector<std::byte> row(static_cast<std::size_t>(frame.width) * kTgaBytesPerPixel);
    const auto source_row_size = static_cast<std::size_t>(frame.width) * kBgraBytesPerPixel;
    for (std::uint32_t row_index = frame.height; row_index-- > 0;) {
        const auto* source = frame.bgra_pixels.get() + static_cast<std::size_t>(row_index) * source_row_size;
        for (std::uint32_t column = 0; column < frame.width; ++column) {
            const auto source_offset = static_cast<std::size_t>(column) * kBgraBytesPerPixel;
            const auto output_offset = static_cast<std::size_t>(column) * kTgaBytesPerPixel;
            std::copy_n(source + source_offset, kTgaBytesPerPixel, row.data() + output_offset);
        }
        file.write(reinterpret_cast<const char*>(row.data()), static_cast<std::streamsize>(row.size()));
    }

    if (!file) {
        throw std::runtime_error("could not finish " + path.filename().string());
    }
    logging::info(kPatchId, "Saved screenshot as " + path.filename().string());
}

std::filesystem::path ScreenshotWriter::next_path() const {
    for (std::uint32_t number = 0; number <= kMaximumScreenshotNumber; ++number) {
        auto candidate = output_directory_ / std::format("screenshot_{:04}.tga", number);
        std::error_code exists_error;
        const auto exists = std::filesystem::exists(candidate, exists_error);
        if (exists_error) {
            throw std::system_error(exists_error, "could not inspect the ScreenShots folder");
        }
        if (!exists) {
            return candidate;
        }
    }
    throw std::runtime_error("all screenshot names from 0000 through 9999 are already in use");
}

} // namespace fusioncutter::patches::screenshot_fix
