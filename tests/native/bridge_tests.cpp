#include "native/bridge.hpp"
#include "hook_test_support.hpp"
#include "plan_test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <vector>

namespace {

using fc::runtime::native::ExecutableBridge;
using fc::runtime::native::FrameInvoker;
using fc::tests::native::CodePage;

constexpr FC_NativeValue kInteger{FC_NATIVE_INTEGER, 4, 4};
constexpr FC_NativeValue kDouble{FC_NATIVE_FLOAT_64, 8, 8};
#if defined(_M_X64)
constexpr auto kArchitecture = FC_ARCH_X64;
constexpr auto kResultRegister = FC_REGISTER_RAX;
#else
constexpr auto kArchitecture = FC_ARCH_X86;
constexpr auto kResultRegister = FC_REGISTER_EAX;
#endif

// Describes physical register storage using an FC_REGISTER_* identifier.
FC_NativeStorage reg(std::uint32_t id) {
    return {FC_NATIVE_STORAGE_REGISTER, id, 0};
}

// Describes a byte offset after the return address and, on x64, the home space.
FC_NativeStorage stack(std::uint32_t offset) {
    return {FC_NATIVE_STORAGE_STACK, FC_REGISTER_NONE, offset};
}

// Writes a weighted sum from two logical int arguments; the frame must provide int result storage.
void FC_CDECL dispatch_sum(void *, const FC_NativeFrame *frame, std::uintptr_t) noexcept {
    // Compute independently of the physical target so the test can expose marshalling errors in either direction.
    *static_cast<int *>(frame->result) =
        *static_cast<const int *>(frame->arguments[0]) * 3 + *static_cast<const int *>(frame->arguments[1]);
}

// Holds a record result larger than the direct result registers can hold.
struct Triple {
    // Distinct words reveal truncated copies and misplaced hidden output pointers.
    std::uint64_t first, second, third;
};

// Fills the caller's Triple with both inputs and a sentinel in the final word.
void __cdecl fill(Triple *result, std::uint32_t first, std::uint32_t second) noexcept {
    *result = {first, second, 0x123456789abcdef0ULL};
}

// Fills the frame's Triple result from two logical uint32_t arguments.
void FC_CDECL dispatch_record(void *, const FC_NativeFrame *frame, std::uintptr_t) noexcept {
    fill(static_cast<Triple *>(frame->result), *static_cast<const std::uint32_t *>(frame->arguments[0]),
         *static_cast<const std::uint32_t *>(frame->arguments[1]));
}

} // namespace

TEST_CASE("A custom native hidden result writes the full structure", "[native][bridge]") {
    // The hidden output pointer occupies the first compiler argument position on each architecture.
#if defined(_M_X64)
    const std::array arguments{FC_NativeArgument{kInteger, reg(FC_REGISTER_RDX)}, FC_NativeArgument{kInteger, reg(FC_REGISTER_R8)}};
    const auto hidden = reg(FC_REGISTER_RCX);
    constexpr std::uint32_t cleanup = FC_STACK_CLEANUP_NONE, extent = 0;
#else
    const std::array arguments{FC_NativeArgument{kInteger, stack(4)}, FC_NativeArgument{kInteger, stack(8)}};
    const auto hidden = stack(0);
    constexpr std::uint32_t cleanup = FC_STACK_CLEANUP_CALLER, extent = 12;
#endif
    const FC_NativeCall call{
        kArchitecture, FC_CALL_HIDDEN_RESULT, {FC_NATIVE_RECORD, sizeof(Triple), alignof(Triple)}, hidden, arguments.data(), 2, cleanup,
        extent};

    // Drive the physical target from a logical frame and verify every result word reaches the caller's object.
    auto invoker = ExecutableBridge::invoker(call);
    REQUIRE(invoker.has_value());

    const std::uint32_t first = 19, second = 27;
    const std::array<const void *, 2> values{&first, &second};
    Triple result{};
    const FC_NativeFrame frame{values.data(), &result};

    reinterpret_cast<FrameInvoker>(invoker->address())(reinterpret_cast<std::uintptr_t>(&fill), &frame);

    CHECK(result.first == 19);
    CHECK(result.second == 27);
    CHECK(result.third == 0x123456789abcdef0ULL);

    // Drive the reverse direction with a compiler call whose explicit first argument is the output pointer.
    auto entry = ExecutableBridge::entry(call, nullptr, &dispatch_record);
    REQUIRE(entry.has_value());

    reinterpret_cast<void(__cdecl *)(Triple *, std::uint32_t, std::uint32_t)>(entry->address())(&result, 31, 43);

    CHECK(result.first == 31);
    CHECK(result.second == 43);
    CHECK(result.third == 0x123456789abcdef0ULL);
}

TEST_CASE("Native entries capture unusual register inputs from an independent caller", "[native][bridge]") {
#if defined(_M_X64)
    const std::array arguments{FC_NativeArgument{kInteger, reg(FC_REGISTER_R12)}, FC_NativeArgument{kInteger, reg(FC_REGISTER_R9)}};
    // Save R12 and supply ordinary x64 home space before converting the two compiler arguments to custom registers.
    std::vector<std::uint8_t> wrapper{0x41, 0x54, 0x48, 0x83, 0xec, 0x20, 0x41, 0x89, 0xcc, 0x41, 0x89, 0xd1, 0x48, 0xb8};
#else
    const std::array arguments{FC_NativeArgument{kInteger, reg(FC_REGISTER_ESI)}, FC_NativeArgument{kInteger, reg(FC_REGISTER_EDX)}};
    // Save ESI and move the compiler's stack arguments into the custom ESI and EDX input registers.
    std::vector<std::uint8_t> wrapper{0x56, 0x8b, 0x74, 0x24, 0x08, 0x8b, 0x54, 0x24, 0x0c, 0xb8};
#endif
    const FC_NativeCall call{kArchitecture, 0, kInteger, reg(kResultRegister), arguments.data(), 2, FC_STACK_CLEANUP_NONE, 0};
    auto entry = ExecutableBridge::entry(call, nullptr, &dispatch_sum);
    REQUIRE(entry.has_value());

    // Embed the generated entry address, then append a call and the wrapper's register/stack restoration.
    const auto address = entry->address();
    const auto *bytes = reinterpret_cast<const std::uint8_t *>(&address);
    wrapper.insert(wrapper.end(), bytes, bytes + sizeof(address));
#if defined(_M_X64)
    wrapper.insert(wrapper.end(), {0xff, 0xd0, 0x48, 0x83, 0xc4, 0x20, 0x41, 0x5c, 0xc3});
#else
    wrapper.insert(wrapper.end(), {0xff, 0xd0, 0x5e, 0xc3});
#endif

    CodePage caller;
    caller.put(0, wrapper);
    caller.seal();

    CHECK(reinterpret_cast<int(__cdecl *)(int, int)>(caller.address())(17, 25) == 76);
}

namespace {

// Weights each argument by position so misplaced register or stack inputs change the result.
int __cdecl six_arguments(int a, int b, int c, int d, int e, int f) noexcept {
    return a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f;
}
#if defined(_M_IX86)
// Computes the callback's weighted sum through a compiler target that removes its own stack arguments.
int __stdcall callee_sum(int left, int right) noexcept {
    return left * 3 + right;
}
#endif
} // namespace

TEST_CASE("Native invokers marshal arguments across registers and stack", "[native][bridge]") {
    // x64 uses four argument registers before stack slots; x86 places all six arguments on the stack.
    std::array<FC_NativeArgument, 6> arguments{};
    for (std::uint32_t index = 0; index < arguments.size(); ++index) {
#if defined(_M_X64)
        constexpr std::array registers{FC_REGISTER_RCX, FC_REGISTER_RDX, FC_REGISTER_R8, FC_REGISTER_R9};
        arguments[index] = {kInteger, index < 4 ? reg(registers[index]) : stack((index - 4) * 8)};
#else
        arguments[index] = {kInteger, stack(index * 4)};
#endif
    }

    const FC_NativeCall call{kArchitecture,
                             FC_CALL_COMPILER,
                             kInteger,
                             reg(kResultRegister),
                             arguments.data(),
                             6,
                             kArchitecture == FC_ARCH_X86 ? FC_STACK_CLEANUP_CALLER : FC_STACK_CLEANUP_NONE,
                             kArchitecture == FC_ARCH_X86 ? 24U : 16U};

    auto bridge = ExecutableBridge::invoker(call);
    REQUIRE(bridge.has_value());

    // Distinct weighted inputs make register and stack argument order observable in one compiler-generated target.
    const std::array values{1, 2, 3, 4, 5, 6};
    std::array<const void *, 6> pointers{};
    for (std::size_t index = 0; index < values.size(); ++index) {
        pointers[index] = &values[index];
    }

    int result{};
    const FC_NativeFrame frame{pointers.data(), &result};

    reinterpret_cast<FrameInvoker>(bridge->address())(reinterpret_cast<std::uintptr_t>(&six_arguments), &frame);

    CHECK(result == 91);
}

TEST_CASE("Native invokers preserve inputs that share result storage", "[native][bridge]") {
    // Compute 3*a+b+c+d+e+int(f) from scratch registers, a stack value and XMM3; SIMD loading must preserve RAX/EAX inputs.
#if defined(_M_X64)
    constexpr std::array<std::uint8_t, 20> code{0x8d, 0x04, 0x40, 0x03, 0xc6, 0x03, 0xc7, 0x03, 0xc1, 0x03,
                                                0x44, 0x24, 0x28, 0xf2, 0x0f, 0x2c, 0xd3, 0x03, 0xc2, 0xc3};
    const std::array arguments{FC_NativeArgument{kInteger, reg(FC_REGISTER_RAX)},
                               FC_NativeArgument{kInteger, reg(FC_REGISTER_RSI)},
                               FC_NativeArgument{kInteger, reg(FC_REGISTER_RDI)},
                               FC_NativeArgument{kInteger, reg(FC_REGISTER_RCX)},
                               FC_NativeArgument{kInteger, stack(0)},
                               FC_NativeArgument{kDouble, reg(FC_REGISTER_XMM3)}};
    constexpr auto cleanup = FC_STACK_CLEANUP_NONE;
    constexpr std::uint32_t extent = 8;
#else
    constexpr std::array<std::uint8_t, 20> code{0x8d, 0x04, 0x40, 0x03, 0xc6, 0x03, 0xc7, 0x03, 0xc1, 0x03,
                                                0x44, 0x24, 0x04, 0xf2, 0x0f, 0x2c, 0xd3, 0x03, 0xc2, 0xc3};
    const std::array arguments{FC_NativeArgument{kInteger, reg(FC_REGISTER_EAX)},
                               FC_NativeArgument{kInteger, reg(FC_REGISTER_ESI)},
                               FC_NativeArgument{kInteger, reg(FC_REGISTER_EDI)},
                               FC_NativeArgument{kInteger, reg(FC_REGISTER_ECX)},
                               FC_NativeArgument{kInteger, stack(0)},
                               FC_NativeArgument{kDouble, reg(FC_REGISTER_XMM3)}};
    constexpr auto cleanup = FC_STACK_CLEANUP_CALLER;
    constexpr std::uint32_t extent = 4;
#endif
    const FC_NativeCall call{kArchitecture, 0, kInteger, reg(kResultRegister), arguments.data(), 6, cleanup, extent};
    auto invoker = ExecutableBridge::invoker(call);
    REQUIRE(invoker.has_value());
    CodePage target;
    target.put(0, code);
    target.seal();

    int shared = 19;
    const int other = 27;
    const double floating = 11.0;
    const std::array<const void *, 6> values{&shared, &other, &shared, &shared, &shared, &floating};
    const FC_NativeFrame frame{values.data(), &shared};
    reinterpret_cast<FrameInvoker>(invoker->address())(target.address(), &frame);

    CHECK(shared == 152);
    CHECK(other == 27);
}

#if defined(_M_IX86)
TEST_CASE("Native x86 callee cleanup balances invoker and entry stacks", "[native][bridge]") {
    const std::array arguments{FC_NativeArgument{kInteger, stack(0)}, FC_NativeArgument{kInteger, stack(4)}};
    const FC_NativeCall call{
        kArchitecture, FC_CALL_COMPILER, kInteger, reg(kResultRegister), arguments.data(), 2, FC_STACK_CLEANUP_CALLEE, 8};
    auto invoker = ExecutableBridge::invoker(call);
    auto entry = ExecutableBridge::entry(call, nullptr, &dispatch_sum);
    REQUIRE(invoker.has_value());
    REQUIRE(entry.has_value());

    const int left = 7, right = 8;
    const std::array<const void *, 2> values{&left, &right};
    int result{};
    const FC_NativeFrame frame{values.data(), &result};

    // Repeated native calls expose stack drift in either generated cleanup direction.
    for (unsigned index = 0; index != 3; ++index) {
        reinterpret_cast<FrameInvoker>(invoker->address())(reinterpret_cast<std::uintptr_t>(&callee_sum), &frame);
        CHECK(result == 29);
        CHECK(reinterpret_cast<int(__stdcall *)(int, int)>(entry->address())(left, right) == 29);
    }
}
#else

namespace {

constexpr DWORD kBridgeException = 0xe1234567;

// Raises the test's SEH exception from a compiler-generated native target.
void __cdecl raise_native() noexcept {
    RaiseException(kBridgeException, 0, 0, nullptr);
}

// Raises the same SEH exception from an entry callback so unwinding crosses the generated entry.
void FC_CDECL raise_dispatch(void *, const FC_NativeFrame *, std::uintptr_t) noexcept {
    raise_native();
}

// Calls through the invoker and returns the caught test exception code, or zero if the target returns.
// Keep SEH handling in this helper so the caller can retain C++ bridge owners while Windows unwinds generated code.
DWORD unwind_invoker(FrameInvoker invoker, const FC_NativeFrame *frame) {
    __try {
        invoker(reinterpret_cast<std::uintptr_t>(&raise_native), frame);
    } __except (GetExceptionCode() == kBridgeException ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return GetExceptionCode();
    }
    return 0;
}

// Calls the entry's void/no-argument convention and returns the caught test exception code, or zero if it returns.
DWORD unwind_entry(std::uintptr_t address) {
    __try {
        reinterpret_cast<void(__cdecl *)()>(address)();
    } __except (GetExceptionCode() == kBridgeException ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return GetExceptionCode();
    }
    return 0;
}
} // namespace

TEST_CASE("Native x64 bridges register executable unwind descriptions", "[native][bridge]") {
    const FC_NativeCall call{kArchitecture, 0, {FC_NATIVE_VOID, 0, 0}, {}, nullptr, 0, FC_STACK_CLEANUP_NONE, 0};
    auto invoker = ExecutableBridge::invoker(call);
    auto entry = ExecutableBridge::entry(call, nullptr, &raise_dispatch);
    REQUIRE(invoker.has_value());
    REQUIRE(entry.has_value());

    // Confirm Windows can discover the emitted table before exercising exception traversal through both directions.
    DWORD64 image_base{};
    REQUIRE(RtlLookupFunctionEntry(invoker->address(), &image_base, nullptr) != nullptr);
    CHECK(image_base == invoker->address());

    const FC_NativeFrame frame{};
    CHECK(unwind_invoker(reinterpret_cast<FrameInvoker>(invoker->address()), &frame) == kBridgeException);
    CHECK(unwind_entry(entry->address()) == kBridgeException);
}

#endif

namespace {

// Supplies the same scalar through a compiler-generated target and an independent frame callback.
template <class T> struct ScalarResult {
    // Uses a fractional float or distinct high and low integer bytes to expose truncated results.
    static T __cdecl native() noexcept {
        if constexpr (std::is_floating_point_v<T>) {
            return static_cast<T>(19.75);
        } else {
            return static_cast<T>(0x1122334455667788ULL);
        }
    }

    // Writes only the declared result extent into the entry bridge's private frame.
    static void FC_CDECL dispatch(void *, const FC_NativeFrame *frame, std::uintptr_t) noexcept {
        *static_cast<T *>(frame->result) = native();
    }
};

// Exercises both bridge directions and checks that the invoker writes only the caller's result bytes.
template <class T> void check_scalar_result(FC_NativeValue value, std::uint32_t result_register) {
    const FC_NativeCall call{kArchitecture, 0, value, reg(result_register), nullptr, 0, FC_STACK_CLEANUP_NONE, 0};
    auto invoker = ExecutableBridge::invoker(call);
    auto entry = ExecutableBridge::entry(call, nullptr, &ScalarResult<T>::dispatch);
    REQUIRE(invoker);
    REQUIRE(entry);

    struct Output {
        T value{};
        std::array<std::uint8_t, 16> following;
    } output;

    output.following.fill(0xa5);
    const FC_NativeFrame frame{nullptr, &output.value};
    reinterpret_cast<FrameInvoker>(invoker->address())(reinterpret_cast<std::uintptr_t>(&ScalarResult<T>::native), &frame);
    CHECK(output.value == ScalarResult<T>::native());
    CHECK(std::ranges::all_of(output.following, [](auto byte) {
        return byte == 0xa5;
    }));
    CHECK(reinterpret_cast<T(__cdecl *)()>(entry->address())() == ScalarResult<T>::native());
}
} // namespace

TEST_CASE("Native bridges preserve narrow, wide and floating scalar results", "[native][bridge]") {
    check_scalar_result<std::uint8_t>({FC_NATIVE_INTEGER, 1, 1}, kResultRegister);
    // An eight-byte integer uses EDX:EAX on x86 and RAX on x64.
    check_scalar_result<std::uint64_t>({FC_NATIVE_INTEGER, 8, 8}, kResultRegister);
#if defined(_M_X64)
    constexpr auto floating_register = FC_REGISTER_XMM0;
#else
    constexpr auto floating_register = FC_REGISTER_ST0;
#endif
    check_scalar_result<double>({FC_NATIVE_FLOAT_64, 8, 8}, floating_register);
}

namespace {

// Requires logical storage alignment stronger than the platform's ordinary 16-byte stack alignment.
struct alignas(64) LargeRecord {
    // Eight KiB force physical stack copies and entry captures to span multiple guard pages.
    std::array<std::uint32_t, 2048> values;
};

// The physical helper uses an ordinary aggregate; the explicit description requests stronger logical-copy alignment.
struct PhysicalLargeRecord {
    // Match LargeRecord's bytes while retaining the compiler's ordinary aggregate alignment.
    std::array<std::uint32_t, 2048> values;
};

// Returns the record's endpoint sum so the test can check both ends of the copied argument.
[[maybe_unused]] int __cdecl consume_large(PhysicalLargeRecord input) noexcept {
    return static_cast<int>(input.values.front() + input.values.back());
}

// Reads one LargeRecord argument, writes its endpoint sum to an int result, and reports alignment through a bool context.
void FC_CDECL dispatch_large(void *context, const FC_NativeFrame *frame, std::uintptr_t) noexcept {
    const auto *input = static_cast<const LargeRecord *>(frame->arguments[0]);
    *static_cast<bool *>(context) = reinterpret_cast<std::uintptr_t>(input) % alignof(LargeRecord) == 0;
    *static_cast<int *>(frame->result) = static_cast<int>(input->values.front() + input->values.back());
}
} // namespace

TEST_CASE("Native bridges align logical records and probe large generated frames", "[native][bridge]") {
    // Entry captures require 64-byte alignment; both directions reserve enough stack to cross guard pages.
    const std::array arguments{FC_NativeArgument{{FC_NATIVE_RECORD, sizeof(LargeRecord), alignof(LargeRecord)}, stack(0)}};
    const FC_NativeCall call{kArchitecture,
                             0,
                             kInteger,
                             reg(kResultRegister),
                             arguments.data(),
                             1,
                             kArchitecture == FC_ARCH_X86 ? FC_STACK_CLEANUP_CALLER : FC_STACK_CLEANUP_NONE,
                             sizeof(LargeRecord)};

    auto invoker = ExecutableBridge::invoker(call);
    bool aligned{};
    auto entry = ExecutableBridge::entry(call, &aligned, &dispatch_large);
    REQUIRE(invoker.has_value());
    REQUIRE(entry.has_value());

    // Endpoint sentinels detect incorrect record extents without checking unrelated middle bytes.
    LargeRecord input{};
    input.values.front() = 17;
    input.values.back() = 25;
    const std::array<const void *, 1> values{&input};
    int result{};
    const FC_NativeFrame frame{values.data(), &result};

#if defined(_M_X64)
    // The physical target reads the custom stack record directly, independently of generated marshalling.
    const std::array<std::uint8_t, 14> target_code{0x8b, 0x44, 0x24, 0x28, 0x8b, 0x8c, 0x24, 0x24, 0x20, 0, 0, 0x01, 0xc8, 0xc3};
    CodePage target;
    target.put(0, target_code);
    target.seal();
    reinterpret_cast<FrameInvoker>(invoker->address())(target.address(), &frame);

    CHECK(result == 42);

    // An independent caller probes its outgoing stack, copies the record and calls the custom entry.
    const std::uint8_t caller_code[]{0x56, 0x57, 0x49, 0x89, 0xd3, 0x48, 0x81, 0xec, 0,    0x10, 0,    0,    0x85, 0x04, 0x24,
                                     0x48, 0x81, 0xec, 0,    0x10, 0,    0,    0x85, 0x04, 0x24, 0x48, 0x83, 0xec, 0x28, 0x48,
                                     0x89, 0xce, 0x48, 0x8d, 0x7c, 0x24, 0x20, 0xb9, 0,    0x20, 0,    0,    0xf3, 0xa4, 0x41,
                                     0xff, 0xd3, 0x48, 0x81, 0xc4, 0x28, 0x20, 0,    0,    0x5f, 0x5e, 0xc3};
    CodePage caller;
    caller.put(0, caller_code);
    caller.seal();
    CHECK(reinterpret_cast<int(__cdecl *)(const PhysicalLargeRecord *, std::uintptr_t)>(caller.address())(
              reinterpret_cast<const PhysicalLargeRecord *>(&input), entry->address()) == 42);
#else
    reinterpret_cast<FrameInvoker>(invoker->address())(reinterpret_cast<std::uintptr_t>(&consume_large), &frame);

    CHECK(result == 42);
    CHECK(reinterpret_cast<int(__cdecl *)(PhysicalLargeRecord)>(entry->address())(PhysicalLargeRecord{input.values}) == 42);
#endif
    CHECK(aligned);
}

TEST_CASE("Native SDK calls code that uses a custom register convention") {
    CodePage code;
    // The fixture adds arguments from deliberately nonstandard registers, then returns the sum in EAX/RAX.
#if defined(_WIN64)
    const std::uint8_t body[]{0x44, 0x89, 0xe0, 0x44, 0x01, 0xc8, 0xc3};
    using Call =
        fc::NativeCall<int(int, int) noexcept, fc::abi::X64<fc::abi::Args<fc::abi::R12, fc::abi::R9>, fc::abi::Result<fc::abi::Rax>>>;
#else
    const std::uint8_t body[]{0x89, 0xf0, 0x01, 0xd0, 0xc3};
    using Call =
        fc::NativeCall<int(int, int) noexcept, fc::abi::X86<fc::abi::Args<fc::abi::Esi, fc::abi::Edx>, fc::abi::Result<fc::abi::Eax>>>;
#endif
    code.put(0x100, body);
    code.seal();

    fc::runtime::SubmittedPlan plan{nullptr, {code.image()}};
    fc::tests::native::PlanView view{plan};
    auto function = view.sdk.function<Call>(0x100);
    CHECK_FALSE(function.call(13, 29));
    REQUIRE(plan.prepare());

    REQUIRE(function.call(13, 29) == 42);
}
