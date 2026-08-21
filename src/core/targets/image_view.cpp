#include "image_view.hpp"

#include <Windows.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

namespace fc::targets {

// The erased owner holds exactly one storage mode and releases a live module reference only for live images.
struct OwnedImage::Owner {
    enum class Storage {
        Mapped,
        PrivateNative,
        LiveModule,
    };

    ImageInfo info;
    std::vector<std::byte> mapped_bytes;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> writable_ranges;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> readable_ranges;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> executable_ranges;
    HMODULE module{};
    void* private_native{};
    Storage storage{Storage::Mapped};

    ~Owner() {
        if (storage == Storage::LiveModule && module != nullptr) {
            FreeLibrary(module);
        }
        if (storage == Storage::PrivateNative && private_native != nullptr) {
            VirtualFree(private_native, 0, MEM_RELEASE);
        }
    }
};

namespace {

// Forms a bounded span without trusting the storage mode or allowing RVA arithmetic to wrap.
template <class Owner>
[[nodiscard]] std::expected<std::span<const std::byte>, ImageReadError> checked_range(const Owner& owner, Rva rva,
                                                                                      std::size_t size) noexcept {
    const auto offset = static_cast<std::size_t>(rva.value);
    if (offset > std::numeric_limits<std::size_t>::max() - size) {
        return std::unexpected(ImageReadError::Overflow);
    }
    if (offset > owner.info.size || size > owner.info.size - offset) {
        return std::unexpected(ImageReadError::OutOfBounds);
    }
    const auto* base = owner.storage == Owner::Storage::Mapped ? owner.mapped_bytes.data()
                                                               : reinterpret_cast<const std::byte*>(owner.info.base);
    return std::span{base + offset, size};
}

// Confirms every virtual memory region covering a live range is committed and permits the required access.
[[nodiscard]] bool live_range_has_access(std::span<const std::byte> range, bool writable) noexcept {
    auto current = reinterpret_cast<std::uintptr_t>(range.data());
    const auto end = current + range.size();
    // Advance by VirtualQuery region boundaries because one reviewed range may cross pages with different protection.
    while (current < end) {
        MEMORY_BASIC_INFORMATION memory{};
        if (VirtualQuery(reinterpret_cast<const void*>(current), &memory, sizeof(memory)) == 0 ||
            memory.State != MEM_COMMIT || memory.Protect == PAGE_NOACCESS || (memory.Protect & PAGE_GUARD) != 0) {
            return false;
        }
        if (!writable) {
            constexpr DWORD readable_protections = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                                                   PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
            if ((memory.Protect & readable_protections) == 0) {
                return false;
            }
        } else {
            constexpr DWORD writable_protections =
                PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
            if ((memory.Protect & writable_protections) == 0) {
                return false;
            }
        }
        const auto region_base = reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
        const auto region_size = static_cast<std::uintptr_t>(memory.RegionSize);
        if (region_base > current || region_size > std::numeric_limits<std::uintptr_t>::max() - region_base) {
            return false;
        }
        const auto region_end = region_base + region_size;
        if (region_end <= current) {
            return false;
        }
        current = std::min(end, region_end);
    }
    return true;
}

} // namespace

std::expected<void, ImageReadError> OwnedImage::read_owned(const void* source, Rva rva,
                                                           std::span<std::byte> output) noexcept {
    const auto& owner = *static_cast<const Owner*>(source);
    auto input = checked_range(owner, rva, output.size());
    if (!input) {
        return std::unexpected(input.error());
    }
    // The reviewed PE policy is authoritative even when the backing mapping contains additional readable bytes.
    const auto end = static_cast<std::uint64_t>(rva.value) + output.size();
    const bool reviewed_readable = std::ranges::any_of(owner.readable_ranges, [&](const auto& range) {
        return rva.value >= range.first && end <= static_cast<std::uint64_t>(range.first) + range.second;
    });
    if (!reviewed_readable) {
        return std::unexpected(ImageReadError::Unreadable);
    }
    // Validate every covered virtual memory region before memcpy can observe a partial or inaccessible range.
    if (owner.storage != OwnedImage::Owner::Storage::Mapped && !live_range_has_access(*input, false)) {
        return std::unexpected(ImageReadError::Unreadable);
    }
    std::memcpy(output.data(), input->data(), output.size());
    return {};
}

bool OwnedImage::writable_owned(const void* source, Rva rva, std::size_t size) noexcept {
    const auto& owner = *static_cast<const Owner*>(source);
    if (!checked_range(owner, rva, size)) {
        return false;
    }
    // A live page becoming writable never expands the reviewed PE section policy for that image profile.
    const auto end = static_cast<std::uint64_t>(rva.value) + size;
    const bool reviewed_writable = std::ranges::any_of(owner.writable_ranges, [&](const auto& range) {
        return rva.value >= range.first && end <= static_cast<std::uint64_t>(range.first) + range.second;
    });
    if (!reviewed_writable) {
        return false;
    }
    if (owner.storage != Owner::Storage::Mapped) {
        return live_range_has_access(*checked_range(owner, rva, size), true);
    }
    return true;
}

// Executability is checked separately because PAGE_EXECUTE is valid for instruction fetch but not ordinary reads.
[[nodiscard]] bool live_range_is_executable(std::span<const std::byte> range) noexcept {
    auto current = reinterpret_cast<std::uintptr_t>(range.data());
    const auto end = current + range.size();
    while (current < end) {
        MEMORY_BASIC_INFORMATION memory{};
        if (VirtualQuery(reinterpret_cast<const void*>(current), &memory, sizeof(memory)) == 0 ||
            memory.State != MEM_COMMIT || (memory.Protect & PAGE_GUARD) != 0) {
            return false;
        }
        constexpr DWORD executable_protections =
            PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        if ((memory.Protect & executable_protections) == 0) {
            return false;
        }
        const auto region_base = reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
        const auto region_size = static_cast<std::uintptr_t>(memory.RegionSize);
        if (region_base > current || region_size > std::numeric_limits<std::uintptr_t>::max() - region_base) {
            return false;
        }
        const auto region_end = region_base + region_size;
        if (region_end <= current) {
            return false;
        }
        current = std::min(end, region_end);
    }
    return true;
}

bool OwnedImage::readable_owned(const void* source, Rva rva, std::size_t size) noexcept {
    const auto& owner = *static_cast<const Owner*>(source);
    const auto range = checked_range(owner, rva, size);
    if (!range) {
        return false;
    }
    const auto end = static_cast<std::uint64_t>(rva.value) + size;
    const bool reviewed_readable = std::ranges::any_of(owner.readable_ranges, [&](const auto& policy) {
        return rva.value >= policy.first && end <= static_cast<std::uint64_t>(policy.first) + policy.second;
    });
    return reviewed_readable && (owner.storage == Owner::Storage::Mapped || live_range_has_access(*range, false));
}

bool OwnedImage::executable_owned(const void* source, Rva rva, std::size_t size) noexcept {
    const auto& owner = *static_cast<const Owner*>(source);
    const auto range = checked_range(owner, rva, size);
    if (!range) {
        return false;
    }
    const auto end = static_cast<std::uint64_t>(rva.value) + size;
    const bool reviewed_executable = std::ranges::any_of(owner.executable_ranges, [&](const auto& policy) {
        return rva.value >= policy.first && end <= static_cast<std::uint64_t>(policy.first) + policy.second;
    });
    return reviewed_executable && (owner.storage == Owner::Storage::Mapped || live_range_is_executable(*range));
}

const ImageInfo& ImageView::info() const noexcept {
    return info_;
}

std::expected<void, ImageReadError> ImageView::read(Rva rva, std::span<std::byte> output) const noexcept {
    return read_ == nullptr ? std::unexpected(ImageReadError::Unreadable) : read_(source_, rva, output);
}

bool ImageView::is_writable(Rva rva, std::size_t size) const noexcept {
    return is_writable_ != nullptr && is_writable_(source_, rva, size);
}

bool ImageView::is_readable(Rva rva, std::size_t size) const noexcept {
    return is_readable_ != nullptr && is_readable_(source_, rva, size);
}

bool ImageView::is_executable(Rva rva, std::size_t size) const noexcept {
    return is_executable_ != nullptr && is_executable_(source_, rva, size);
}

OwnedImage::OwnedImage(std::unique_ptr<Owner> owner) noexcept : owner_(std::move(owner)) {
    refresh_view();
}

OwnedImage::OwnedImage(OwnedImage&& other) noexcept : owner_(std::move(other.owner_)) {
    refresh_view();
    other.view_ = {};
}

OwnedImage& OwnedImage::operator=(OwnedImage&& other) noexcept {
    if (this != &other) {
        owner_ = std::move(other.owner_);
        refresh_view();
        other.view_ = {};
    }
    return *this;
}

OwnedImage::~OwnedImage() = default;

const ImageView& OwnedImage::view() const noexcept {
    return view_;
}

OwnedImage OwnedImage::mapped(ImageInfo info, std::vector<std::byte> bytes,
                              std::vector<std::pair<std::uint32_t, std::uint32_t>> writable_ranges,
                              std::vector<std::pair<std::uint32_t, std::uint32_t>> readable_ranges,
                              std::vector<std::pair<std::uint32_t, std::uint32_t>> executable_ranges) {
    // Repoint ImageInfo at the owned copy so moves cannot leave the view tied to caller storage.
    auto owner = std::make_unique<Owner>();
    owner->info = info;
    owner->mapped_bytes = std::move(bytes);
    owner->info.base = reinterpret_cast<std::uintptr_t>(owner->mapped_bytes.data());
    owner->info.size = owner->mapped_bytes.size();
    owner->writable_ranges = std::move(writable_ranges);
    owner->readable_ranges = std::move(readable_ranges);
    owner->executable_ranges = std::move(executable_ranges);
    return OwnedImage{std::move(owner)};
}

std::expected<OwnedImage, std::string>
OwnedImage::private_native(ImageInfo info, std::vector<std::byte> bytes,
                           std::vector<std::pair<std::uint32_t, std::uint32_t>> writable_ranges,
                           std::vector<std::pair<std::uint32_t, std::uint32_t>> readable_ranges,
                           std::vector<std::pair<std::uint32_t, std::uint32_t>> executable_ranges) {
    // Copy into real virtual memory so tests and verifiers exercise Windows protection changes without a live module.
    auto owner = std::make_unique<Owner>();
    owner->info = info;
    owner->private_native = VirtualAlloc(nullptr, bytes.size(), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (owner->private_native == nullptr) {
        return std::unexpected("VirtualAlloc failed with Windows error " + std::to_string(GetLastError()));
    }
    std::memcpy(owner->private_native, bytes.data(), bytes.size());
    owner->info.base = reinterpret_cast<std::uintptr_t>(owner->private_native);
    owner->info.size = bytes.size();
    owner->writable_ranges = std::move(writable_ranges);
    owner->readable_ranges = std::move(readable_ranges);
    owner->executable_ranges = std::move(executable_ranges);
    owner->storage = Owner::Storage::PrivateNative;
    return OwnedImage{std::move(owner)};
}

OwnedImage OwnedImage::live(ImageInfo info, void* module,
                            std::vector<std::pair<std::uint32_t, std::uint32_t>> writable_ranges,
                            std::vector<std::pair<std::uint32_t, std::uint32_t>> readable_ranges,
                            std::vector<std::pair<std::uint32_t, std::uint32_t>> executable_ranges) {
    // Retain the acquired module reference alongside its reviewed section policy for the entire view lifetime.
    auto owner = std::make_unique<Owner>();
    owner->info = info;
    owner->module = static_cast<HMODULE>(module);
    owner->writable_ranges = std::move(writable_ranges);
    owner->readable_ranges = std::move(readable_ranges);
    owner->executable_ranges = std::move(executable_ranges);
    owner->storage = Owner::Storage::LiveModule;
    return OwnedImage{std::move(owner)};
}

void OwnedImage::refresh_view() noexcept {
    // The erased source pointer must be rebound after every owner move because it addresses the new unique_ptr target.
    if (!owner_) {
        view_ = {};
        return;
    }
    view_.source_ = owner_.get();
    view_.read_ = &OwnedImage::read_owned;
    view_.is_writable_ = &OwnedImage::writable_owned;
    view_.is_readable_ = &OwnedImage::readable_owned;
    view_.is_executable_ = &OwnedImage::executable_owned;
    view_.info_ = owner_->info;
}

} // namespace fc::targets
