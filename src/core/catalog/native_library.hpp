#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <vector>

namespace fc::catalog {

// Names one half-open range inside a loaded image without owning the module that backs it.
struct ImageExtent {
    std::uintptr_t begin{};
    std::uintptr_t end{};
};

// Describes the mapped image and executable sections that may own retained callbacks.
class CodeOwner {
  public:
    // Creates a non-owning image description; the containing module must remain loaded for every later query.
    [[nodiscard]] static std::expected<CodeOwner, std::string> from_address(std::uintptr_t address);
    [[nodiscard]] static std::expected<CodeOwner, std::string> from_module(void* module);

    // Image ranges validate retained ABI tables; executable membership validates callbacks and entry points.
    [[nodiscard]] bool contains_image_range(const void* address, std::size_t size) const noexcept;
    [[nodiscard]] bool contains_executable(std::uintptr_t address) const noexcept;
    // Crash publication reuses the already-validated PE identity instead of inspecting plugin memory later.
    [[nodiscard]] ImageExtent image_extent() const noexcept;
    [[nodiscard]] std::uint32_t timestamp() const noexcept;

  private:
    std::uintptr_t image_begin_{};
    std::uintptr_t image_end_{};
    std::uint32_t timestamp_{};
    std::vector<ImageExtent> executable_sections_;
};

// Move-only owner for one externally loaded plugin and its normalized absolute path.
class NativeLibrary {
  public:
    NativeLibrary() = default;
    NativeLibrary(const NativeLibrary&) = delete;
    NativeLibrary& operator=(const NativeLibrary&) = delete;
    NativeLibrary(NativeLibrary&& other) noexcept;
    NativeLibrary& operator=(NativeLibrary&& other) noexcept;
    ~NativeLibrary();

    // Loads an absolute plugin path; dependent DLLs may resolve only beside it or from System32.
    [[nodiscard]] static std::expected<NativeLibrary, std::string> load(const std::filesystem::path& path);

    [[nodiscard]] const std::filesystem::path& path() const noexcept;
    [[nodiscard]] const CodeOwner& code_owner() const noexcept;
    [[nodiscard]] void* find_export(const char* name) const noexcept;

  private:
    NativeLibrary(void* module, std::filesystem::path path, CodeOwner code_owner) noexcept;
    void reset() noexcept;

    void* module_{};
    std::filesystem::path path_;
    CodeOwner code_owner_;
};

} // namespace fc::catalog
