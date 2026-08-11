#pragma once

#include "outcome.hpp"
#include "target.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
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

    [[nodiscard]] static BytePattern exact(std::span<const std::byte> bytes) noexcept {
        return {bytes, {}};
    }
};

template <typename T>
    requires std::is_trivially_copyable_v<T>
[[nodiscard]] BytePattern exact_pattern(const T& value) noexcept {
    return BytePattern::exact(std::as_bytes(std::span{&value, 1}));
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

struct NearConstraint {
    std::uint32_t rva;
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

    [[nodiscard]] PatchAddress offset(std::size_t byte_offset) const noexcept {
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
        checked_write(operation, rva, exact_pattern(expected), std::as_bytes(std::span{&replacement, 1}));
    }

    void checked_write(std::string_view operation, std::uint32_t rva, BytePattern expected, PatchAddress replacement);

    void nop(std::string_view operation, std::uint32_t rva, BytePattern expected);

    void require_bytes(std::string_view operation, std::uint32_t rva, BytePattern expected);

    template <typename Function>
        requires(std::is_pointer_v<Function> && std::is_function_v<std::remove_pointer_t<Function>>)
    [[nodiscard]] OriginalFunction<Function> inline_hook(std::string_view operation, std::uint32_t rva,
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

    template <typename Destination>
    void redirect_call(std::string_view operation, std::uint32_t rva, BytePattern expected, Destination destination) {
        redirect(operation, rva, expected, RedirectKind::Call, destination);
    }

    template <typename Function>
        requires(std::is_pointer_v<Function> && std::is_function_v<std::remove_pointer_t<Function>>)
    [[nodiscard]] OriginalFunction<Function> redirect_call_with_original(std::string_view operation, std::uint32_t rva,
                                                                         BytePattern expected, Function destination) {
        auto slot = std::make_shared<std::atomic<std::uintptr_t>>(0);
        add_redirect(operation, rva, expected, RedirectKind::Call, PatchAddress::absolute(destination), slot);
        return OriginalFunction<Function>{std::move(slot)};
    }

    template <typename Destination>
    void redirect_jump(std::string_view operation, std::uint32_t rva, BytePattern expected, Destination destination) {
        redirect(operation, rva, expected, RedirectKind::Jump, destination);
    }

    template <typename T>
        requires(std::is_trivially_copyable_v<T> && !std::is_const_v<T> && !std::is_volatile_v<T>)
    [[nodiscard]] AllocatedData<T> allocate_data(std::string_view operation, std::size_t count,
                                                 std::span<const T> initial_values = {},
                                                 std::optional<NearConstraint> proximity = std::nullopt) {
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
                        std::span<const std::byte> initial_values, std::optional<NearConstraint> proximity,
                        std::shared_ptr<std::atomic<std::uintptr_t>> slot);

    friend class PreparedPatchPlan;
    friend class patching_detail::PlanVerification;
};

} // namespace fusioncutter
