#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <span>
#include <string>

namespace fusioncutter::verify {

class MappedImage {
  public:
    MappedImage(const MappedImage&) = delete;
    MappedImage(MappedImage&& other) noexcept = default;
    MappedImage& operator=(const MappedImage&) = delete;
    MappedImage& operator=(MappedImage&& other) noexcept = default;
    ~MappedImage() = default;

    [[nodiscard]] static std::expected<MappedImage, std::string> load(const std::filesystem::path& path);

    [[nodiscard]] std::uintptr_t base() const noexcept;
    [[nodiscard]] std::uintptr_t preferred_base() const noexcept;
    [[nodiscard]] std::span<std::byte> bytes() noexcept;

  private:
    struct AllocationDeleter {
        void operator()(void* allocation) const noexcept;
    };

    MappedImage(void* allocation, std::size_t size, std::uintptr_t preferred_base) noexcept;

    std::unique_ptr<void, AllocationDeleter> allocation_;
    std::size_t size_{};
    std::uintptr_t preferred_base_{};
};

} // namespace fusioncutter::verify
