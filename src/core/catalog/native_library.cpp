#include "native_library.hpp"

#include <Windows.h>

#include <algorithm>
#include <limits>
#include <sstream>
#include <string_view>
#include <system_error>
#include <utility>

namespace fc::catalog {
namespace {

// These helpers keep Windows failures attributable and extent arithmetic for loaded images overflow-safe.
[[nodiscard]] std::string windows_error(std::string_view operation, DWORD error) {
    std::ostringstream output;
    output << operation << " failed with Windows error " << error;
    return output.str();
}

[[nodiscard]] bool checked_add(std::uintptr_t left, std::size_t right, std::uintptr_t& result) noexcept {
    if (right > std::numeric_limits<std::uintptr_t>::max() - left) {
        return false;
    }
    result = left + right;
    return true;
}

} // namespace

std::expected<CodeOwner, std::string> CodeOwner::from_address(std::uintptr_t address) {
    HMODULE module{};
    // CodeOwner is a validator, not a module owner; the real owner controls the lifetime and reference count.
    if (address == 0 ||
        !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(address), &module)) {
        return std::unexpected(windows_error("GetModuleHandleExW", GetLastError()));
    }
    return from_module(module);
}

std::expected<CodeOwner, std::string> CodeOwner::from_module(void* module_value) {
    if (module_value == nullptr) {
        return std::unexpected("A code owner requires a loaded module");
    }

    // Read the mapped headers in place; the caller's module lifetime keeps this non-owning inspection valid.
    const auto module = static_cast<HMODULE>(module_value);
    const auto base = reinterpret_cast<std::uintptr_t>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) {
        return std::unexpected("The loaded code owner has no valid DOS header");
    }

    std::uintptr_t nt_address{};
    if (!checked_add(base, static_cast<std::size_t>(dos->e_lfanew), nt_address)) {
        return std::unexpected("The loaded code owner header address overflows");
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(nt_address);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.NumberOfSections == 0 ||
        nt->OptionalHeader.SizeOfImage == 0) {
        return std::unexpected("The loaded code owner has no valid PE header");
    }

    CodeOwner result;
    result.image_begin_ = base;
    result.timestamp_ = nt->FileHeader.TimeDateStamp;
    if (!checked_add(base, nt->OptionalHeader.SizeOfImage, result.image_end_)) {
        return std::unexpected("The loaded code owner image extent overflows");
    }

    // Extents of loaded images use SizeOfImage and executable section RVAs, not raw on-disk offsets.
    const auto* section = IMAGE_FIRST_SECTION(nt);
    result.executable_sections_.reserve(nt->FileHeader.NumberOfSections);
    for (WORD index = 0; index < nt->FileHeader.NumberOfSections; ++index) {
        const auto& current = section[index];
        if ((current.Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0) {
            continue;
        }
        const auto size = std::max(current.Misc.VirtualSize, current.SizeOfRawData);
        std::uintptr_t begin{};
        std::uintptr_t end{};
        if (!checked_add(base, current.VirtualAddress, begin) || !checked_add(begin, size, end) ||
            end > result.image_end_) {
            return std::unexpected("An executable section lies outside its loaded image");
        }
        result.executable_sections_.push_back({begin, end});
    }
    return result;
}

bool CodeOwner::contains_image_range(const void* address, std::size_t size) const noexcept {
    const auto begin = reinterpret_cast<std::uintptr_t>(address);
    std::uintptr_t end{};
    return begin >= image_begin_ && checked_add(begin, size, end) && end <= image_end_;
}

bool CodeOwner::contains_executable(std::uintptr_t address) const noexcept {
    return std::ranges::any_of(executable_sections_, [address](const ImageExtent& section) {
        return address >= section.begin && address < section.end;
    });
}

ImageExtent CodeOwner::image_extent() const noexcept {
    return {image_begin_, image_end_};
}

std::uint32_t CodeOwner::timestamp() const noexcept {
    return timestamp_;
}

NativeLibrary::NativeLibrary(void* module, std::filesystem::path path, CodeOwner code_owner) noexcept
    : module_(module), path_(std::move(path)), code_owner_(std::move(code_owner)) {}

NativeLibrary::NativeLibrary(NativeLibrary&& other) noexcept
    : module_(std::exchange(other.module_, nullptr)), path_(std::move(other.path_)),
      code_owner_(std::move(other.code_owner_)) {}

NativeLibrary& NativeLibrary::operator=(NativeLibrary&& other) noexcept {
    if (this != &other) {
        reset();
        module_ = std::exchange(other.module_, nullptr);
        path_ = std::move(other.path_);
        code_owner_ = std::move(other.code_owner_);
    }
    return *this;
}

NativeLibrary::~NativeLibrary() {
    reset();
}

std::expected<NativeLibrary, std::string> NativeLibrary::load(const std::filesystem::path& path) {
    // Normalize before loading so diagnostics and retained identity use one unambiguous absolute spelling.
    std::error_code error;
    auto absolute = std::filesystem::absolute(path, error);
    if (error) {
        return std::unexpected("The plugin path could not be made absolute: " + error.message());
    }
    absolute = absolute.lexically_normal();

    // The plugin directory permits reviewed private dependencies; System32 prevents ambient search path loading.
    const auto module =
        LoadLibraryExW(absolute.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (module == nullptr) {
        return std::unexpected(windows_error("LoadLibraryExW", GetLastError()));
    }

    // Record executable extents before publishing the wrapper so callback validation can reject foreign code.
    auto code_owner = CodeOwner::from_module(module);
    if (!code_owner) {
        FreeLibrary(module);
        return std::unexpected(std::move(code_owner.error()));
    }
    return NativeLibrary{module, std::move(absolute), std::move(*code_owner)};
}

const std::filesystem::path& NativeLibrary::path() const noexcept {
    return path_;
}

const CodeOwner& NativeLibrary::code_owner() const noexcept {
    return code_owner_;
}

void* NativeLibrary::find_export(const char* name) const noexcept {
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(module_), name));
}

void NativeLibrary::reset() noexcept {
    if (module_ != nullptr) {
        FreeLibrary(static_cast<HMODULE>(module_));
        module_ = nullptr;
    }
}

} // namespace fc::catalog
