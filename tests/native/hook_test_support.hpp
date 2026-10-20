#pragma once

#include "native/hook_registry.hpp"
#include "native/plan.hpp"
#include "patches/patch_record.hpp"
#include <Windows.h>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstring>
#include <span>

namespace fc::tests::native {

using fc::runtime::HookRegistry;
using fc::runtime::PatchRecord;
using fc::runtime::PlanImage;
using fc::runtime::SubmittedPlan;

constexpr FC_NativeValue kInteger{FC_NATIVE_INTEGER, 4, 4};
// Describes a compiler integer result using this architecture's return register.
#if defined(_M_X64)
constexpr FC_NativeCall kNullary{FC_ARCH_X64, FC_CALL_COMPILER,      kInteger, {FC_NATIVE_STORAGE_REGISTER, FC_REGISTER_RAX, 0}, nullptr,
                                 0,           FC_STACK_CLEANUP_NONE, 0};
#else
constexpr FC_NativeCall kNullary{FC_ARCH_X86, FC_CALL_COMPILER,      kInteger, {FC_NATIVE_STORAGE_REGISTER, FC_REGISTER_EAX, 0}, nullptr,
                                 0,           FC_STACK_CLEANUP_NONE, 0};
#endif

// Owns a 4 KiB mapping for independently supplied machine code used by production hook installation.
class CodePage {
  public:
    // Allocates writable instruction storage and requires allocation to succeed for the test.
    CodePage() {
        memory_ = VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        REQUIRE(memory_ != nullptr);
        // NOP-filled space gives backend detours complete instructions beyond each small test body.
        std::memset(memory_, 0x90, 4096);
    }

    // Releases the mapping after every borrowing plan and physical hook has been destroyed.
    ~CodePage() {
        VirtualFree(memory_, 0, MEM_RELEASE);
    }

    // Copies fixture instructions at a byte offset within the page; call before seal().
    void put(std::uint32_t offset, std::span<const std::uint8_t> bytes) {
        REQUIRE(offset + bytes.size() <= 4096);
        std::memcpy(static_cast<std::uint8_t *>(memory_) + offset, bytes.data(), bytes.size());
    }

    // Close the writable preparation phase and make emitted instructions visible to the processor.
    void seal() {
        DWORD previous{};
        REQUIRE(VirtualProtect(memory_, 4096, PAGE_EXECUTE_READ, &previous));
        REQUIRE(FlushInstructionCache(GetCurrentProcess(), memory_, 4096));
    }

    // Returns a borrowed address at the supplied byte offset; callers keep accesses within the live page.
    std::uintptr_t address(std::uint32_t offset = 0) const {
        return reinterpret_cast<std::uintptr_t>(memory_) + offset;
    }

    // Plans borrow this mapping as one readable executable section; the page must outlive their hooks.
    PlanImage image() const {
        return {"game", "fixture", address(), 4096, {{".text", address(), 4096, IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ}}, {}};
    }

  private:
    void *memory_{};
};

// Names a byte offset in the fixture image binding shared by CodePage and SubmittedPlan.
inline FC_Location location(std::uint32_t offset) {
    return {{"fixture", 7}, offset, {"hook fixture", 12}, 0};
}

// Submits through the ABI boundary and returns the original-call resource when the hook supplies one.
// Callback context must remain alive while installed hooks can invoke it.
inline void *submit_hook(SubmittedPlan &plan, std::uint32_t offset, std::uint32_t scope, const FC_NativeCall *call, void *context,
                         FC_NativeThunk modifier, FC_NativeThunk before, FC_NativeThunk after) {
    FC_Operation operation{};
    operation.kind = FC_OPERATION_HOOK;
    operation.value.hook = {location(offset), {}, scope, call, context, modifier, before, after, nullptr, 0, 0};

    void *resource{};
    const auto submitted = plan.submit(operation, &resource);
    INFO((submitted ? "hook submitted" : submitted.error().message));
    REQUIRE(submitted.has_value());
    return resource;
}

// Complete production preparation before installation and retain diagnostics in failed assertions.
inline void install(SubmittedPlan &plan) {
    const auto prepared = plan.prepare();
    INFO((prepared ? "hook prepared" : prepared.error().message));
    REQUIRE(prepared.has_value());

    const auto installed = plan.install();
    INFO((installed ? "hook installed" : installed.error().message));
    REQUIRE(installed.has_value());
}

} // namespace fc::tests::native
