#include "hook_test_support.hpp"
#include "plan_test_support.hpp"
#include <array>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using fc::tests::native::CodePage;
using fc::tests::native::HookRegistry;
using fc::tests::native::install;
using fc::tests::native::kNullary;
using fc::tests::native::Pages;
using fc::tests::native::PlanImage;
using fc::tests::native::PlanView;
using fc::tests::native::submit_hook;
using fc::tests::native::SubmittedPlan;

namespace {

// Supplies a fixed integer result instead of invoking the callsite's original target.
void FC_CDECL replace_call(void *, const FC_NativeFrame *frame, void *) noexcept {
    *static_cast<int *>(frame->result) = 42;
}

// Emits an independent direct CALL, with x64 shadow space and stack alignment, followed by RET.
void put_caller(CodePage &page, std::uint32_t offset, std::uint32_t target) {
#if defined(_M_X64)
    std::vector<std::uint8_t> code{0x48, 0x83, 0xec, 0x28, 0xe8};
#else
    std::vector<std::uint8_t> code{0xe8};
#endif
    // CALL's signed displacement is measured from the end of its four-byte operand; both offsets share this page.
    const auto relative = static_cast<std::int32_t>(target - (offset + code.size() + 4));
    const auto *bytes = reinterpret_cast<const std::uint8_t *>(&relative);
    code.insert(code.end(), bytes, bytes + sizeof(relative));
#if defined(_M_X64)
    code.insert(code.end(), {0x48, 0x83, 0xc4, 0x28});
#endif
    code.push_back(0xc3);
    page.put(offset, code);
}
} // namespace

TEST_CASE("Callsite hooks retain the original and leave other callers unchanged", "[native][hook]") {
    CodePage code;
    constexpr std::array<std::uint8_t, 6> target{0xb8, 0x09, 0x00, 0x00, 0x00, 0xc3};
    code.put(128, target);
    // Two callers share one target so modifying one CALL can be distinguished from detouring the function.
    put_caller(code, 0, 128);
    put_caller(code, 64, 128);
    code.seal();

    HookRegistry hooks;
    SubmittedPlan plan{nullptr, {code.image()}, &hooks};
#if defined(_M_X64)
    constexpr std::uint32_t site = 4;
#else
    constexpr std::uint32_t site = 0;
#endif

    // Install only the first caller's CALL and retain a handle to its decoded original target.
    auto *original = submit_hook(plan, site, FC_HOOK_CALL, &kNullary, nullptr, &replace_call, nullptr, nullptr);
    install(plan);

    CHECK(reinterpret_cast<int(__cdecl *)()>(code.address())() == 42);
    CHECK(reinterpret_cast<int(__cdecl *)()>(code.address(64))() == 9);
    CHECK(reinterpret_cast<int(__cdecl *)()>(code.address(128))() == 9);

    std::uintptr_t original_address{};
    REQUIRE(fc::runtime::native_api()->resolve(original, &original_address, {}));
    CHECK(reinterpret_cast<int(__cdecl *)()>(original_address)() == 9);

    // Changing the installed CALL must make original_view reject its saved original bytes.
    DWORD previous{};
    REQUIRE(VirtualProtect(reinterpret_cast<void *>(code.address()), 4096, PAGE_EXECUTE_READWRITE, &previous));
    *reinterpret_cast<std::uint8_t *>(code.address(site)) = 0x90;
    REQUIRE(VirtualProtect(reinterpret_cast<void *>(code.address()), 4096, previous, &previous));
    std::vector<std::byte> observed(5);
    CHECK_FALSE(hooks.original_view(code.address(site), observed));
}

TEST_CASE("Published observers expose their owner and prevent ordinary destruction", "[native][hook]") {
    CodePage code;
    constexpr std::array<std::uint8_t, 6> target{0xb8, 0x09, 0x00, 0x00, 0x00, 0xc3};
    code.put(5, target);
    code.seal();

    unsigned observed{}, destroyed{};
    fc::tests::native::PatchRecord initial_owner, additional_owner;
    initial_owner.resolved.owner = "initial.patch";
    additional_owner.resolved.owner = "additional.patch";
    HookRegistry hooks;
    initial_owner.plan = std::make_unique<SubmittedPlan>(&initial_owner, std::vector{code.image()}, &hooks);
    additional_owner.plan = std::make_unique<SubmittedPlan>(&additional_owner, std::vector{code.image()}, &hooks);
    const auto observer = +[](void *context, const FC_NativeFrame *, void *) noexcept {
        ++*static_cast<unsigned *>(context);
    };
    submit_hook(*initial_owner.plan, 0, FC_HOOK_FUNCTION, &kNullary, &observed, nullptr, observer, nullptr);
    install(*initial_owner.plan);

    additional_owner.handler.context = &destroyed;
    additional_owner.handler.destroy = +[](void *context) noexcept {
        ++*static_cast<unsigned *>(context);
    };
    submit_hook(*additional_owner.plan, 0, FC_HOOK_FUNCTION, &kNullary, &observed, nullptr, observer, nullptr);
    REQUIRE(additional_owner.plan->validate());
    auto preparation = hooks.prepare(*additional_owner.plan);
    REQUIRE(preparation);
    REQUIRE(additional_owner.plan->revalidate());

    // Install through the registry to check publication before SubmittedPlan's final exposure mark.
    REQUIRE(hooks.install(*preparation, *additional_owner.plan));
    CHECK(additional_owner.plan->exposed());
    CHECK(reinterpret_cast<int(__cdecl *)()>(code.address())() == 9);
    CHECK(observed == 2);

    additional_owner.destroy_unexposed();
    CHECK(destroyed == 0);
}

TEST_CASE("Instruction observers follow changes to registers, stack and continuation", "[native][hook]") {
    CodePage code;
    // Entry pushes 0x55 and loads five; the alternate continuation adds seven to the modifier's twenty.
    constexpr std::array<std::uint8_t, 14> entry{0x6a, 0x55, 0xb8, 0x05, 0x00, 0x00, 0x00, 0x83, 0xc0, 0x03, 0x90, 0x90, 0x59, 0xc3};
    constexpr std::array<std::uint8_t, 4> alternate{0x83, 0xc0, 0x07, 0xc3};
    code.put(0, entry);
    code.put(32, alternate);
    code.seal();
    const auto continuation = code.address(32);
    std::uintptr_t incoming_stack{};
    unsigned faults{}, before{}, after{};
    HookRegistry hooks;
    SubmittedPlan plan{nullptr, {code.image()}, &hooks};
    PlanView view{plan};

    view.sdk.observe_instruction(7, fc::before([&](const fc::CpuContext &registers) noexcept {
                                     incoming_stack = fc::stack_pointer(registers);
#if defined(_M_X64)
                                     const auto accumulator = registers.rax;
#else
                                     const auto accumulator = registers.eax;
#endif
                                     if (accumulator != 5 || *reinterpret_cast<const std::uintptr_t *>(incoming_stack) != 0x55) {
                                         ++faults;
                                     }
                                     ++before;
                                 }),
                                 fc::after([&](const fc::CpuContext &registers) noexcept {
#if defined(_M_X64)
                                     const auto accumulator = registers.rax, instruction = registers.rip;
#else
                                     const auto accumulator = registers.eax, instruction = registers.eip;
#endif
                                     if (accumulator != 20 || instruction != continuation ||
                                         fc::stack_pointer(registers) != incoming_stack + sizeof(void *)) {
                                         ++faults;
                                     }
                                     ++after;
                                 }));
    view.sdk.hook_instruction(7, [&](fc::CpuContext &registers) noexcept {
    // Remove the pushed sentinel so the alternate continuation's RET sees the original return address.
#if defined(_M_X64)
        registers.rax = 20;
        registers.rsp += sizeof(void *);
        registers.rip = continuation;
#else
        registers.eax = 20;
        registers.esp += sizeof(void *);
        registers.eip = static_cast<std::uint32_t>(continuation);
#endif
    });
    install(plan);
    CHECK(reinterpret_cast<int(__cdecl *)()>(code.address())() == 27);
    CHECK(faults == 0);
    CHECK(before == 1);
    CHECK(after == 1);
}

namespace {

// Matches the fixture's eight-byte record passed and returned by value.
struct NativeSmall {
    std::uint32_t first, second;
};

// Matches the fixture's large record, whose compiler ABI uses indirect return storage.
struct NativeLarge {
    std::uint64_t first, second, third;
};

// Keeps the fixture DLL loaded while plans and hooks borrow its code.
class NativeModule {
  public:
    // Loads the fixture DLL and requires a live module before export lookup or plan construction.
    NativeModule() : value_(LoadLibraryW(std::filesystem::path{FC_NATIVE_HOOK_FIXTURE}.c_str())) {
        REQUIRE(value_ != nullptr);
    }

    // Releases the DLL reference after all plans and hooks borrowing its code have been destroyed.
    ~NativeModule() {
        FreeLibrary(value_);
    }

    // Looks up a fixture export; Function must match its signature, and the module must outlive its use.
    template <class Function> Function function(const char *name) const {
        const auto address = GetProcAddress(value_, name);
        REQUIRE(address != nullptr);
        return reinterpret_cast<Function>(address);
    }

    // Names an export by its RVA within the native_dll plan binding; function must belong to this module.
    template <class Function> fc::Location at(Function function) const {
        return {"native_dll",
                static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(function) - reinterpret_cast<std::uintptr_t>(value_))};
    }

    // Returns the loaded DLL's image facts; the module must outlive plans and hooks borrowing the image.
    PlanImage image() const {
        // Read the loaded PE headers to supply the module size and section table.
        const auto base = reinterpret_cast<std::uintptr_t>(value_);
        const auto &dos = *reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
        const auto &headers = *reinterpret_cast<const IMAGE_NT_HEADERS *>(base + static_cast<std::uintptr_t>(dos.e_lfanew));

        PlanImage result{"game", "native_dll", base, headers.OptionalHeader.SizeOfImage};
        const auto *sections = IMAGE_FIRST_SECTION(&headers);
        // PE section names occupy at most eight bytes; retain each section's mapped extent and access flags.
        for (unsigned index = 0; index < headers.FileHeader.NumberOfSections; ++index) {
            const auto &section = sections[index];
            std::string name{reinterpret_cast<const char *>(section.Name), IMAGE_SIZEOF_SHORT_NAME};
            name.resize(name.find('\0') == std::string::npos ? name.size() : name.find('\0'));
            result.sections.push_back({std::move(name), base + section.VirtualAddress, section.Misc.VirtualSize, section.Characteristics});
        }

        return result;
    }

  private:
    HMODULE value_{};
};

} // namespace

TEST_CASE("Native DLL hooks retain originals and support structure and variadic calls", "[native][hook][dll]") {
    NativeModule module;
    using Add = int(__cdecl *)(int, int);
    using Small = NativeSmall(__cdecl *)(NativeSmall);
    using Large = NativeLarge(__cdecl *)(NativeLarge, int);
    using Variadic = int(__cdecl *)(int, ...);
    const auto add = module.function<Add>("fc_native_add");
    const auto small = module.function<Small>("fc_native_small");
    const auto large = module.function<Large>("fc_native_large");
    const auto variadic = module.function<Variadic>("fc_native_variadic");

    HookRegistry hooks;
    SubmittedPlan plan{nullptr, {module.image()}, &hooks};
    PlanView view{plan};
    unsigned before{}, after{}, faults{};
    fc::NativeFunction<Add> original_add;

    // The observer sees original arguments before the modifier and its replacement result afterward.
    view.sdk.observe<Add>(module.at(add), fc::before([&](int left, int right) noexcept {
                              if (left != 7 || right != 5) {
                                  ++faults;
                              }
                              ++before;
                          }),
                          fc::after([&](int left, int right, int result) noexcept {
                              if (left != 7 || right != 5 || result != 126) {
                                  ++faults;
                              }
                              ++after;
                          }));

    // Call the retained original from inside the modifier, then add a visible replacement contribution.
    original_add = view.sdk.hook<Add>(module.at(add), [&](int left, int right) noexcept {
        const auto result = original_add.call(left, right);
        if (!result) {
            ++faults;
            return -1;
        }
        return *result + 100;
    });

    // These handles exercise compiler structure and variadic calls alongside the shared physical hook.
    const auto direct_small = view.sdk.function<Small>(module.at(small));
    const auto direct_large = view.sdk.function<Large>(module.at(large));
    const auto direct_variadic = view.sdk.function<Variadic>(module.at(variadic));
    REQUIRE_FALSE(plan.error());

    install(plan);

    // Calling the export traverses the hook; calling its original handle bypasses observers and the modifier.
    CHECK(add(7, 5) == 126);
    CHECK(original_add.call(7, 5) == 26);

    const auto small_result = direct_small.call(NativeSmall{3, 4});
    REQUIRE(small_result.has_value());
    CHECK(small_result->first == 10);
    CHECK(small_result->second == 13);

    const auto large_result = direct_large.call(NativeLarge{7, 13, 29}, 5);
    REQUIRE(large_result.has_value());
    CHECK(large_result->first == 12);
    CHECK(large_result->second == 18);
    CHECK(large_result->third == 34);

    CHECK(direct_variadic.call(4, 2, 3, 5, 7) == 17);
    CHECK(before == 1);
    CHECK(after == 1);
    CHECK(faults == 0);
}

TEST_CASE("Native SDK hooks compiler calls with integer and floating-point arguments") {
    Pages pages;
    // This independent native function adds an integer to a double using each architecture's compiler ABI.
#if defined(_WIN64)
    const std::uint8_t code[]{0xf2, 0x0f, 0x2a, 0xc1, 0xf2, 0x0f, 0x58, 0xc1, 0xc3};
#else
    const std::uint8_t code[]{0xdd, 0x44, 0x24, 0x08, 0xdb, 0x44, 0x24, 0x04, 0xde, 0xc1, 0xc3};
#endif
    std::memcpy(pages.base + 0x100, code, sizeof(code));
    using Call = double(__cdecl *)(int, double);
    auto target = reinterpret_cast<Call>(pages.base + 0x100);

    fc::runtime::HookRegistry hooks;
    fc::runtime::SubmittedPlan plan{nullptr, {pages.image()}, &hooks};
    PlanView view{plan};
    fc::NativeFunction<Call> original;

    // The callback borrows original by reference; assign the prepared handle before installing the hook.
    original = view.sdk.hook<Call>(0x100, [&](int integer, double floating) noexcept {
        auto result = original.call(integer, floating);
        return result ? *result + 0.5 : -1.0;
    });
    REQUIRE_FALSE(plan.error());
    REQUIRE(plan.prepare());
    REQUIRE(plan.install());

    REQUIRE(target(4, 2.25) == 6.75);
}
