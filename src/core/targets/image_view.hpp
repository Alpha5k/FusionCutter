#pragma once

#include <FusionCutter/Abi.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fc::targets {

// An RVA is interpreted only against the ImageView that receives it.
struct Rva {
    std::uint32_t value;
};

// Describes one recognized image while keeping its backing storage behind ImageView.
struct ImageInfo {
    FC_TargetImage image{};
    // Profile text belongs to the process-lifetime framework profile catalog.
    std::string_view profile;
    std::uintptr_t base{};
    std::size_t size{};
};

// Read failures distinguish arithmetic errors, access policy violations, and inaccessible live pages.
enum class ImageReadError {
    Overflow,
    OutOfBounds,
    Unreadable,
};

// Non-owning type erasure keeps live and verifier reads on one path; its OwnedImage must outlive every use.
class ImageView {
  public:
    // Reads copy from the image and never expose a pointer into mutable or unloadable storage.
    [[nodiscard]] const ImageInfo& info() const noexcept;
    // Both operations require the complete range to satisfy reviewed PE policy and current backing access.
    [[nodiscard]] std::expected<void, ImageReadError> read(Rva rva, std::span<std::byte> output) const noexcept;
    [[nodiscard]] bool is_readable(Rva rva, std::size_t size) const noexcept;
    [[nodiscard]] bool is_writable(Rva rva, std::size_t size) const noexcept;
    [[nodiscard]] bool is_executable(Rva rva, std::size_t size) const noexcept;

  private:
    friend class OwnedImage;

    using ReadFunction = std::expected<void, ImageReadError> (*)(const void*, Rva, std::span<std::byte>) noexcept;
    using WritableFunction = bool (*)(const void*, Rva, std::size_t) noexcept;

    const void* source_{};
    ReadFunction read_{};
    WritableFunction is_writable_{};
    WritableFunction is_readable_{};
    WritableFunction is_executable_{};
    ImageInfo info_{};
};

// The erased owner keeps live module references and verifier mappings behind the same immutable view contract.
class OwnedImage {
  public:
    OwnedImage() = default;
    OwnedImage(const OwnedImage&) = delete;
    OwnedImage& operator=(const OwnedImage&) = delete;
    OwnedImage(OwnedImage&& other) noexcept;
    OwnedImage& operator=(OwnedImage&& other) noexcept;
    ~OwnedImage();

    // The returned view remains stable only until this owner is moved or destroyed.
    [[nodiscard]] const ImageView& view() const noexcept;

    // Test and verifier construction owns a mapped copy plus its reviewed readable and writable ranges.
    [[nodiscard]] static OwnedImage mapped(ImageInfo info, std::vector<std::byte> bytes,
                                           std::vector<std::pair<std::uint32_t, std::uint32_t>> writable_ranges,
                                           std::vector<std::pair<std::uint32_t, std::uint32_t>> readable_ranges = {},
                                           std::vector<std::pair<std::uint32_t, std::uint32_t>> executable_ranges = {});

    // Native integration tests use real pages so protection changes exercise the live memory path.
    [[nodiscard]] static std::expected<OwnedImage, std::string>
    private_native(ImageInfo info, std::vector<std::byte> bytes,
                   std::vector<std::pair<std::uint32_t, std::uint32_t>> writable_ranges,
                   std::vector<std::pair<std::uint32_t, std::uint32_t>> readable_ranges = {},
                   std::vector<std::pair<std::uint32_t, std::uint32_t>> executable_ranges = {});

    // Live construction adopts one GetModuleHandleEx reference and releases it with the owner.
    [[nodiscard]] static OwnedImage live(ImageInfo info, void* module,
                                         std::vector<std::pair<std::uint32_t, std::uint32_t>> writable_ranges,
                                         std::vector<std::pair<std::uint32_t, std::uint32_t>> readable_ranges,
                                         std::vector<std::pair<std::uint32_t, std::uint32_t>> executable_ranges);

  private:
    struct Owner;

    explicit OwnedImage(std::unique_ptr<Owner> owner) noexcept;
    [[nodiscard]] static std::expected<void, ImageReadError> read_owned(const void* source, Rva rva,
                                                                        std::span<std::byte> output) noexcept;
    [[nodiscard]] static bool writable_owned(const void* source, Rva rva, std::size_t size) noexcept;
    [[nodiscard]] static bool readable_owned(const void* source, Rva rva, std::size_t size) noexcept;
    [[nodiscard]] static bool executable_owned(const void* source, Rva rva, std::size_t size) noexcept;
    void refresh_view() noexcept;

    std::unique_ptr<Owner> owner_;
    ImageView view_;
};

} // namespace fc::targets
