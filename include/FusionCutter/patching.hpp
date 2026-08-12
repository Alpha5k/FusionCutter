#pragma once

#include "outcome.hpp"
#include "target.hpp"

#include <array>
#include <atomic>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>

namespace fusioncutter {

namespace patching_detail {
class PlanStorage;
class PlanVerification;
} // namespace patching_detail

template <std::uint8_t... Values>
[[nodiscard]] consteval std::array<std::byte, sizeof...(Values)> byte_array() noexcept {
    return {std::byte{Values}...};
}

struct BytePattern {
    std::span<const std::byte> bytes;
    // An empty mask means exact matching. Otherwise, only set mask bits are compared.
    std::span<const std::byte> mask;

    [[nodiscard]] static constexpr BytePattern exact(std::span<const std::byte> bytes) noexcept {
        return {bytes, {}};
    }

    [[nodiscard]] static constexpr BytePattern masked(std::span<const std::byte> bytes,
                                                      std::span<const std::byte> mask) noexcept {
        return {bytes, mask};
    }
};

// Keeps a native RVA and its owned expected pattern together in target layout data.
template <std::size_t Size> struct NativeSite {
    std::uint32_t rva;
    std::array<std::byte, Size> expected;
    std::optional<std::array<std::byte, Size>> mask;

    [[nodiscard]] BytePattern pattern() const noexcept {
        return mask.has_value() ? BytePattern::masked(expected, *mask) : BytePattern::exact(expected);
    }
};

// Embeds one fixed-width value into a native byte pattern with compile-time bounds checking.
template <std::size_t Offset, std::size_t Size, typename Value>
    requires std::is_trivially_copyable_v<Value>
void embed_value(std::array<std::byte, Size>& bytes, const Value& value) noexcept {
    static_assert(Offset + sizeof(Value) <= Size);
    std::memcpy(bytes.data() + Offset, &value, sizeof(value));
}

// Embeds an architecture-sized absolute address resolved from the selected image.
template <std::size_t Offset, std::size_t Size>
void embed_image_address(std::array<std::byte, Size>& bytes, const ImageContext& image, std::uint32_t rva) noexcept {
    const auto address = image.address_at_rva(rva);
    assert(address != 0);
    embed_value<Offset>(bytes, address);
}

// Embeds a signed displacement relative to the supplied native base RVA.
template <std::size_t Offset, std::signed_integral Displacement = std::int32_t, std::size_t Size>
void embed_relative_displacement(std::array<std::byte, Size>& bytes, std::uint32_t displacement_base_rva,
                                 std::uint32_t target_rva) noexcept {
    const auto difference = static_cast<std::int64_t>(target_rva) - static_cast<std::int64_t>(displacement_base_rva);
    assert(std::in_range<Displacement>(difference));
    embed_value<Offset>(bytes, static_cast<Displacement>(difference));
}

// Read an unaligned field from a verified native object or stack frame.
template <typename Value>
    requires std::is_trivially_copyable_v<Value>
[[nodiscard]] Value read_native_field(const void* object, std::size_t offset = 0) noexcept {
    Value value{};
    std::memcpy(&value, static_cast<const std::byte*>(object) + offset, sizeof(value));
    return value;
}

// Write an unaligned runtime field after the patch has validated its native owner.
template <typename Value>
    requires std::is_trivially_copyable_v<Value>
void write_native_field(void* object, std::size_t offset, const Value& value) noexcept {
    std::memcpy(static_cast<std::byte*>(object) + offset, &value, sizeof(value));
}

template <typename Value>
    requires std::is_trivially_copyable_v<Value>
void write_native_field(void* object, const Value& value) noexcept {
    write_native_field(object, 0, value);
}

class PatchAddress {
  public:
    PatchAddress() = default;

    template <typename Pointer>
        requires std::is_pointer_v<Pointer>
    [[nodiscard]] static PatchAddress absolute(Pointer pointer) noexcept {
        PatchAddress address;
        address.kind_ = Kind::Absolute;
        address.value_ = reinterpret_cast<std::uintptr_t>(pointer);
        return address;
    }

    [[nodiscard]] static PatchAddress image_rva(std::uint32_t rva) noexcept;

  private:
    enum class Kind {
        Invalid,
        Absolute,
        ImageRva,
        Allocation,
    };

    Kind kind_{Kind::Invalid};
    std::uintptr_t value_{};
    std::shared_ptr<std::atomic<std::uintptr_t>> allocation_;

    [[nodiscard]] static PatchAddress allocation(std::shared_ptr<std::atomic<std::uintptr_t>> slot,
                                                 std::size_t byte_offset) noexcept;

    friend class PatchPlan;
    friend class PreparedPatchPlan;
    friend class patching_detail::PlanStorage;

    template <typename T> friend class AllocatedData;
};

template <typename Function>
    requires(std::is_pointer_v<Function> && std::is_function_v<std::remove_pointer_t<Function>>)
class OriginalFunction {
  public:
    OriginalFunction() = default;

    [[nodiscard]] Function get() const noexcept {
        if (!slot_) {
            return nullptr;
        }
        return reinterpret_cast<Function>(slot_->load(std::memory_order_acquire));
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return get() != nullptr;
    }

  private:
    explicit OriginalFunction(std::shared_ptr<std::atomic<std::uintptr_t>> slot) noexcept : slot_(std::move(slot)) {}

    std::shared_ptr<std::atomic<std::uintptr_t>> slot_;

    friend class PatchPlan;
};

union SimdRegister {
    std::uint8_t u8[16];
    std::uint16_t u16[8];
    std::uint32_t u32[4];
    std::uint64_t u64[2];
    float f32[4];
    double f64[2];
};

#if defined(_M_X64)
struct MidHookContext {
    SimdRegister xmm0, xmm1, xmm2, xmm3, xmm4, xmm5, xmm6, xmm7;
    SimdRegister xmm8, xmm9, xmm10, xmm11, xmm12, xmm13, xmm14, xmm15;
    std::uintptr_t rflags, r15, r14, r13, r12, r11, r10, r9, r8;
    std::uintptr_t rdi, rsi, rdx, rcx, rbx, rax, rbp, rsp, trampoline_rsp, rip;
};
#elif defined(_M_IX86)
struct MidHookContext {
    SimdRegister xmm0, xmm1, xmm2, xmm3, xmm4, xmm5, xmm6, xmm7;
    std::uintptr_t eflags, edi, esi, edx, ecx, ebx, eax, ebp, esp, trampoline_esp, eip;
};
#else
#error Fusion Cutter supports only x86 and x64 targets.
#endif

using MidHookCallback = void (*)(MidHookContext& context) noexcept;

enum class RedirectKind {
    Call,
    Jump,
};

struct AllocationProximity {
    std::uint32_t anchor_rva;
    std::size_t maximum_distance{0x7FFF'FFFF};
};

template <typename T> class AllocatedData {
  public:
    AllocatedData() = default;

    [[nodiscard]] T* data() const noexcept {
        if (!slot_) {
            return nullptr;
        }
        return reinterpret_cast<T*>(slot_->load(std::memory_order_acquire));
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return count_;
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return slot_ != nullptr;
    }

    [[nodiscard]] PatchAddress base() const noexcept {
        return PatchAddress::allocation(slot_, 0);
    }

    [[nodiscard]] PatchAddress element(std::size_t index) const noexcept {
        if (index >= count_ || index > std::numeric_limits<std::size_t>::max() / sizeof(T)) {
            return {};
        }
        return PatchAddress::allocation(slot_, index * sizeof(T));
    }

    [[nodiscard]] PatchAddress byte_offset(std::size_t byte_offset) const noexcept {
        return PatchAddress::allocation(slot_, byte_offset);
    }

  private:
    AllocatedData(std::shared_ptr<std::atomic<std::uintptr_t>> slot, std::size_t count) noexcept
        : slot_(std::move(slot)), count_(count) {}

    std::shared_ptr<std::atomic<std::uintptr_t>> slot_;
    std::size_t count_{};

    friend class PatchPlan;
};

class PatchPlan {
  public:
    PatchPlan(PatchId patch_id, ImageContext image);
    PatchPlan(const PatchPlan&) = delete;
    PatchPlan(PatchPlan&&) noexcept;
    PatchPlan& operator=(const PatchPlan&) = delete;
    PatchPlan& operator=(PatchPlan&&) noexcept;
    ~PatchPlan();

    void checked_write(std::string_view operation, std::uint32_t rva, BytePattern expected,
                       std::span<const std::byte> replacement);

    template <typename T>
        requires std::is_trivially_copyable_v<T>
    void checked_write(std::string_view operation, std::uint32_t rva, const T& expected, const T& replacement) {
        checked_write(operation, rva, BytePattern::exact(std::as_bytes(std::span{&expected, 1})),
                      std::as_bytes(std::span{&replacement, 1}));
    }

    void checked_write(std::string_view operation, std::uint32_t rva, BytePattern expected, PatchAddress replacement);

    template <typename T>
        requires std::is_trivially_copyable_v<T>
    void checked_write(std::string_view operation, std::uint32_t rva, const T& expected, PatchAddress replacement) {
        checked_write(operation, rva, BytePattern::exact(std::as_bytes(std::span{&expected, 1})),
                      std::move(replacement));
    }

    void nop(std::string_view operation, std::uint32_t rva, BytePattern expected);

    void require_bytes(std::string_view operation, std::uint32_t rva, BytePattern expected);

    template <typename T>
        requires std::is_trivially_copyable_v<T>
    void require_bytes(std::string_view operation, std::uint32_t rva, const T& expected) {
        require_bytes(operation, rva, BytePattern::exact(std::as_bytes(std::span{&expected, 1})));
    }

    template <typename Function>
        requires(std::is_pointer_v<Function> && std::is_function_v<std::remove_pointer_t<Function>>)
    void inline_hook(std::string_view operation, std::uint32_t rva, BytePattern expected, Function destination) {
        add_inline_hook(operation, rva, expected, reinterpret_cast<std::uintptr_t>(destination), {});
    }

    template <typename Function>
        requires(std::is_pointer_v<Function> && std::is_function_v<std::remove_pointer_t<Function>>)
    [[nodiscard]] OriginalFunction<Function> inline_hook_with_original(std::string_view operation, std::uint32_t rva,
                                                                       BytePattern expected, Function destination) {
        auto slot = std::make_shared<std::atomic<std::uintptr_t>>(0);
        add_inline_hook(operation, rva, expected, reinterpret_cast<std::uintptr_t>(destination), slot);
        return OriginalFunction<Function>{std::move(slot)};
    }

    void mid_hook(std::string_view operation, std::uint32_t rva, BytePattern expected, MidHookCallback callback);

    void redirect(std::string_view operation, std::uint32_t rva, BytePattern expected, RedirectKind kind,
                  PatchAddress destination);

    template <typename Function>
        requires(std::is_pointer_v<Function> && std::is_function_v<std::remove_pointer_t<Function>>)
    void redirect(std::string_view operation, std::uint32_t rva, BytePattern expected, RedirectKind kind,
                  Function destination) {
        redirect(operation, rva, expected, kind, PatchAddress::absolute(destination));
    }

    template <typename Function>
        requires(std::is_pointer_v<Function> && std::is_function_v<std::remove_pointer_t<Function>>)
    [[nodiscard]] OriginalFunction<Function> redirect_with_original(std::string_view operation, std::uint32_t rva,
                                                                    BytePattern expected, RedirectKind kind,
                                                                    PatchAddress destination) {
        auto slot = std::make_shared<std::atomic<std::uintptr_t>>(0);
        add_redirect(operation, rva, expected, kind, std::move(destination), slot);
        return OriginalFunction<Function>{std::move(slot)};
    }

    template <typename Function>
        requires(std::is_pointer_v<Function> && std::is_function_v<std::remove_pointer_t<Function>>)
    [[nodiscard]] OriginalFunction<Function> redirect_with_original(std::string_view operation, std::uint32_t rva,
                                                                    BytePattern expected, RedirectKind kind,
                                                                    Function destination) {
        return redirect_with_original<Function>(operation, rva, expected, kind, PatchAddress::absolute(destination));
    }

    template <typename Destination>
    void redirect_call(std::string_view operation, std::uint32_t rva, BytePattern expected, Destination destination) {
        redirect(operation, rva, expected, RedirectKind::Call, destination);
    }

    template <typename Function>
        requires(std::is_pointer_v<Function> && std::is_function_v<std::remove_pointer_t<Function>>)
    [[nodiscard]] OriginalFunction<Function> redirect_call_with_original(std::string_view operation, std::uint32_t rva,
                                                                         BytePattern expected, Function destination) {
        return redirect_with_original(operation, rva, expected, RedirectKind::Call, destination);
    }

    template <typename Function>
        requires(std::is_pointer_v<Function> && std::is_function_v<std::remove_pointer_t<Function>>)
    [[nodiscard]] OriginalFunction<Function> redirect_call_with_original(std::string_view operation, std::uint32_t rva,
                                                                         BytePattern expected,
                                                                         PatchAddress destination) {
        return redirect_with_original<Function>(operation, rva, expected, RedirectKind::Call, std::move(destination));
    }

    template <typename Destination>
    void redirect_jump(std::string_view operation, std::uint32_t rva, BytePattern expected, Destination destination) {
        redirect(operation, rva, expected, RedirectKind::Jump, destination);
    }

    template <typename Function>
        requires(std::is_pointer_v<Function> && std::is_function_v<std::remove_pointer_t<Function>>)
    [[nodiscard]] OriginalFunction<Function> redirect_jump_with_original(std::string_view operation, std::uint32_t rva,
                                                                         BytePattern expected, Function destination) {
        return redirect_with_original(operation, rva, expected, RedirectKind::Jump, destination);
    }

    template <typename Function>
        requires(std::is_pointer_v<Function> && std::is_function_v<std::remove_pointer_t<Function>>)
    [[nodiscard]] OriginalFunction<Function> redirect_jump_with_original(std::string_view operation, std::uint32_t rva,
                                                                         BytePattern expected,
                                                                         PatchAddress destination) {
        return redirect_with_original<Function>(operation, rva, expected, RedirectKind::Jump, std::move(destination));
    }

    template <typename T>
        requires(std::is_trivially_copyable_v<T> && !std::is_const_v<T> && !std::is_volatile_v<T>)
    [[nodiscard]] AllocatedData<T> allocate_data(std::string_view operation, std::size_t count,
                                                 std::span<const T> initial_values = {},
                                                 std::optional<AllocationProximity> proximity = std::nullopt) {
        auto slot = std::make_shared<std::atomic<std::uintptr_t>>(0);
        add_allocation(operation, count, sizeof(T), alignof(T), std::as_bytes(initial_values), proximity, slot);
        return AllocatedData<T>{std::move(slot), count};
    }

  private:
    std::unique_ptr<patching_detail::PlanStorage> storage_;

    void add_inline_hook(std::string_view operation, std::uint32_t rva, BytePattern expected,
                         std::uintptr_t destination, std::shared_ptr<std::atomic<std::uintptr_t>> original_slot);
    void add_redirect(std::string_view operation, std::uint32_t rva, BytePattern expected, RedirectKind kind,
                      PatchAddress destination, std::shared_ptr<std::atomic<std::uintptr_t>> original_slot = {});
    void add_allocation(std::string_view operation, std::size_t count, std::size_t element_size, std::size_t alignment,
                        std::span<const std::byte> initial_values, std::optional<AllocationProximity> proximity,
                        std::shared_ptr<std::atomic<std::uintptr_t>> slot);

    friend class PreparedPatchPlan;
    friend class patching_detail::PlanVerification;
};

} // namespace fusioncutter
