#pragma once

#include <fc/base.hpp>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace fc {

// Saved registers for this build's architecture, supplied to instruction hook callbacks.
using CpuContext = FC_CpuContext;

// Returns the saved stack address at the observed instruction.
constexpr std::uint32_t stack_pointer(const FC_CpuContext32 &context) noexcept {
    return context.esp;
}

// Returns the saved stack address at the observed instruction on x64.
constexpr std::uint64_t stack_pointer(const FC_CpuContext64 &context) noexcept {
    return context.rsp;
}

// Returns the saved frame address at the observed instruction.
constexpr std::uint32_t frame_pointer(const FC_CpuContext32 &context) noexcept {
    return context.ebp;
}

// Returns the saved frame address at the observed instruction on x64.
constexpr std::uint64_t frame_pointer(const FC_CpuContext64 &context) noexcept {
    return context.rbp;
}

// Identifies a native site by selected image profile and offset from its loaded base.
struct Location {
    // Selected profile ID, such as BF2.GOG.59EDF52B; a requirement label is resolved through Plan::for_image.
    std::string image;
    // Relative virtual address (RVA) in bytes from the image base.
    std::uint32_t rva{};
    // Optional diagnostic label; it does not participate in address resolution.
    std::string name;
    // Set by failed derivations so submission rejects the location instead of using a partial offset.
    bool invalid{};

    // Borrows the owned strings until this location is changed or destroyed.
    FC_Location view() const {
        return {detail::text(image), rva, detail::text(name), invalid};
    }
};

// Evidence owns its input bytes so temporaries remain usable until submission.
struct Evidence {
    // Selects an FC_EvidenceKind comparison performed during planning and revalidation.
    std::uint32_t kind{};
    // Masked comparisons select bits with the corresponding mask byte; exact comparisons use all bits.
    std::vector<std::uint8_t> bytes, mask;
    // Expected image destination for pointer, direct CALL or direct JMP evidence.
    Location target;

    // Borrows this object's byte vectors and target strings until submission copies them.
    FC_Evidence view() const {
        return {kind, {bytes.data(), detail::count(bytes.size())}, {mask.data(), detail::count(mask.size())}, target.view()};
    }
};

// Copy expected original bytes; submission rejects empty evidence and requires every supplied byte to match.
inline Evidence exact_bytes(std::span<const std::uint8_t> bytes) {
    return {FC_EVIDENCE_EXACT_BYTES, {bytes.begin(), bytes.end()}};
}

// Copy expected original bytes from either byte spans or temporary initializer lists.
inline Evidence exact_bytes(Bytes bytes) {
    return exact_bytes(std::span{reinterpret_cast<const std::uint8_t *>(bytes.data()), bytes.size()});
}

inline Evidence exact_bytes(std::initializer_list<std::uint8_t> bytes) {
    return exact_bytes(std::span{bytes.begin(), bytes.size()});
}

// Copy expected bytes and compare only bits selected by mask; both inputs must be nonempty and equally sized.
inline Evidence masked_bytes(std::span<const std::uint8_t> bytes, std::span<const std::uint8_t> mask) {
    return {FC_EVIDENCE_MASKED_BYTES, {bytes.begin(), bytes.end()}, {mask.begin(), mask.end()}};
}

// Copy byte spans or temporary initializer lists into the same masked comparison.
inline Evidence masked_bytes(Bytes bytes, Bytes mask) {
    return masked_bytes(std::span{reinterpret_cast<const std::uint8_t *>(bytes.data()), bytes.size()},
                        std::span{reinterpret_cast<const std::uint8_t *>(mask.data()), mask.size()});
}

inline Evidence masked_bytes(std::initializer_list<std::uint8_t> bytes, std::initializer_list<std::uint8_t> mask) {
    return masked_bytes({bytes.begin(), bytes.size()}, {mask.begin(), mask.size()});
}

// Require a trivially copyable object's complete byte representation, including padding, to match the original native bytes.
template <class T> Evidence expect(const T &value) {
    static_assert(std::is_trivially_copyable_v<T>);
    return exact_bytes({reinterpret_cast<const std::uint8_t *>(&value), sizeof(T)});
}

// Require a pointer-sized native field to contain the selected target's absolute address.
inline Evidence points_to(Location target) {
    return {FC_EVIDENCE_POINTS_TO, {}, {}, std::move(target)};
}

// Require a five-byte direct relative CALL whose decoded destination is target.
inline Evidence direct_call_to(Location target) {
    return {FC_EVIDENCE_DIRECT_CALL_TO, {}, {}, std::move(target)};
}

// Require a five-byte direct relative JMP whose decoded destination is target.
inline Evidence direct_jump_to(Location target) {
    return {FC_EVIDENCE_DIRECT_JUMP_TO, {}, {}, std::move(target)};
}

// Describes a destination in a selected image, this plan's allocation or the submitting plugin's code.
struct Address {
    // Used with evidence for FC_ADDRESS_IMAGE.
    Location location;
    Evidence evidence;
    // Opaque Data resource used for FC_ADDRESS_ALLOCATION; offset is measured in bytes.
    void *resource{};
    std::uint64_t offset{};
    // Absolute process address used for FC_ADDRESS_FUNCTION.
    std::uintptr_t function{};
    std::uint32_t kind = FC_ADDRESS_IMAGE;
    bool invalid{};

    // Borrows image strings and evidence bytes; the runtime copies them during submission.
    FC_Address view() const {
        return {kind, location.view(), evidence.view(), resource, offset, function, invalid};
    }
};

// Describe an image destination with evidence checked independently of the edit's source evidence.
inline Address address(Location target, Evidence evidence = {}) {
    return {std::move(target), std::move(evidence)};
}

// Describe executable code owned by the submitting plugin; runtime submission checks module ownership.
template <class Function>
    requires std::is_pointer_v<Function> && std::is_function_v<std::remove_pointer_t<Function>>
Address address(Function target) {
    Address result;
    result.kind = FC_ADDRESS_FUNCTION;
    result.function = reinterpret_cast<std::uintptr_t>(target);
    return result;
}

// Selects a four-byte signed displacement relative to the end of the field written by Plan::write.
struct RelativeAddress {
    Address target;
};

// Wrap a destination for a four-byte relative address write.
inline RelativeAddress rel32(Address target) {
    return {std::move(target)};
}

// Preserves integer signedness when selecting a decoded instruction operand's replacement value.
template <class T> struct Immediate {
    T value;
};

// Wrap an integer for Plan::operand; submission checks that it fits the instruction's encoded field.
template <class T> Immediate<T> immediate(T value) {
    static_assert(std::is_integral_v<T>);
    return {value};
}

// Controls runtime allocation placement for replacement data.
struct Placement {
    // Byte alignment; zero selects alignof(T), and explicit alignment must be a power of two.
    std::uint32_t alignment{};
    // Optional selected profile ID requesting allocation within relative-address reach of that image.
    std::string near_image;
};

// Describes Count typed elements at an RVA in the Plan view's bound image, with optional original-byte evidence.
template <class T, std::size_t Count = 1> struct DataLocation {
    using value_type = T;
    static constexpr auto count = Count;
    static_assert(Count > 0);

    std::uint32_t rva{};
    std::string name;
    Evidence evidence;
    bool invalid{};
};

// Describes an instruction site in the Plan view's bound image, retaining evidence and derivation failures.
struct CodeLocation {
    std::uint32_t rva{};
    std::string name;
    Evidence evidence;
    bool invalid{};
};

// Supplies the table's starting RVA for typed vtable_slot derivation.
using VtableLocation = CodeLocation;

// Describes a function entry and its call signature in the Plan view's bound image.
template <class Call> struct FunctionLocation {
    std::uint32_t rva{};
    std::string name;
    Evidence evidence;
};

// Describes a direct CALL instruction and its callee signature in the Plan view's bound image.
template <class Call> struct CallLocation {
    std::uint32_t rva{};
    std::string name;
    Evidence evidence;
};

// Marks a callback to run before the intercepted call or instruction hook's modifier.
template <class F> struct Before {
    F callback;
};

// Marks a callback to run after the intercepted call or instruction hook's modifier.
template <class F> struct After {
    F callback;
};

// Retain the callable as a before observer; Plan checks its noexcept signature when adapting it.
template <class F> Before<F> before(F callback) {
    return {std::move(callback)};
}

// Retain the callable as an after observer; Plan checks its noexcept signature when adapting it.
template <class F> After<F> after(F callback) {
    return {std::move(callback)};
}

} // namespace fc
