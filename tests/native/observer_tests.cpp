#include "hook_test_support.hpp"
#include "plan_test_support.hpp"
#include <array>
#include <atomic>
#include <semaphore>
#include <thread>
#include <vector>
#include <Winsock2.h>
#include <intrin.h>

using fc::tests::native::CodePage;
using fc::tests::native::HookRegistry;
using fc::tests::native::install;
using fc::tests::native::kNullary;
using fc::tests::native::PatchRecord;
using fc::tests::native::PlanView;
using fc::tests::native::submit_hook;
using fc::tests::native::SubmittedPlan;

namespace {

using Unary = int(__cdecl *)(int);

// Each participant receives an independently aligned object for the lifetime of one invocation.
struct alignas(64) ObserverState {
    std::array<std::uint64_t, 8> values{0, 42};
};

// Nested entries must preserve their enclosing callback's state on the same thread.
thread_local const ObserverState *outer_state{};

} // namespace

TEST_CASE("Function observers retain paired state through recursion and late publication", "[native][hook]") {
    CodePage code;
#if defined(_M_X64)
    constexpr std::array<std::uint8_t, 4> body{0x8d, 0x41, 0x01, 0xc3};
#else
    constexpr std::array<std::uint8_t, 6> body{0x8b, 0x44, 0x24, 0x04, 0x40, 0xc3};
#endif
    code.put(5, body);
    code.seal();
    const auto function = reinterpret_cast<Unary>(code.address());
    std::atomic<unsigned> faults{}, before{}, after{}, late_before{}, late_after{};
    std::binary_semaphore entered{0}, resume{0};
    std::vector<unsigned> before_order, after_order;
    PatchRecord base_owner, late_owner;
    base_owner.resolved.owner = "Zulu.patch";
    late_owner.resolved.owner = "alpha.patch";
    HookRegistry hooks;
    SubmittedPlan initial{&base_owner, {code.image()}, &hooks}, additional{&late_owner, {code.image()}, &hooks};
    PlanView first{initial}, late{additional};

    // Distinct participant markers reveal overlapping state; recursion and concurrent calls require fresh objects.
    for (unsigned index = 0; index != 2; ++index) {
        first.sdk.observe<Unary, ObserverState>(0, fc::before([&, index](int argument, ObserverState &state) noexcept {
                                                    if (reinterpret_cast<std::uintptr_t>(&state) % alignof(ObserverState) ||
                                                        state.values[1] != 42 || state.values.back()) {
                                                        ++faults;
                                                    }
                                                    state.values.front() = argument;
                                                    state.values.back() = 100 + index;
                                                    ++before;
                                                    if (argument == 79) {
                                                        before_order.push_back(index);
                                                    }
                                                    if (index == 0 && argument == 77) {
                                                        // Hold an invocation on the old participant list while another plan publishes its
                                                        // callbacks.
                                                        entered.release();
                                                        resume.acquire();
                                                    }
                                                    if (index == 0 && argument == 2) {
                                                        outer_state = &state;
                                                        if (function(1) != 2) {
                                                            ++faults;
                                                        }
                                                        outer_state = nullptr;
                                                    } else if (index == 0 && argument == 1 && outer_state == &state) {
                                                        ++faults;
                                                    }
                                                }),
                                                fc::after([&, index](int argument, int result, const ObserverState &state) noexcept {
                                                    if (state.values.front() != static_cast<std::uint64_t>(argument) ||
                                                        state.values.back() != 100 + index || result != argument + 1) {
                                                        ++faults;
                                                    }
                                                    ++after;
                                                    if (argument == 79) {
                                                        after_order.push_back(index);
                                                    }
                                                }));
    }
    install(initial);
    CHECK(function(2) == 3);

    late.sdk.observe<Unary>(0, fc::before([&](int argument) noexcept {
                                ++late_before;
                                if (argument == 79) {
                                    before_order.push_back(2);
                                }
                            }),
                            fc::after([&](int argument, int) noexcept {
                                ++late_after;
                                if (argument == 79) {
                                    after_order.push_back(2);
                                }
                            }));
    REQUIRE(additional.prepare());
    std::atomic<int> blocked_result{};
    std::thread blocked{[&] {
        blocked_result = function(77);
    }};
    entered.acquire();
    const auto installed = additional.install();
    resume.release();
    blocked.join();
    REQUIRE(installed);
    CHECK(blocked_result == 78);
    CHECK(late_before == 0);
    CHECK(late_after == 0);

    // Owner IDs sort ASCII case-insensitively; after callbacks reverse the captured before order.
    CHECK(function(79) == 80);
    CHECK(before_order == std::vector<unsigned>{2, 0, 1});
    CHECK(after_order == std::vector<unsigned>{1, 0, 2});

    std::array<std::thread, 2> threads;
    for (unsigned worker = 0; worker < threads.size(); ++worker) {
        threads[worker] = std::thread{[&, worker] {
            for (int index = 0; index != 2; ++index) {
                const int argument = 1000 + static_cast<int>(worker) * 100 + index;
                if (function(argument) != argument + 1) {
                    ++faults;
                }
            }
        }};
    }
    for (auto &thread : threads) {
        thread.join();
    }
    CHECK(faults == 0);
    CHECK(before == 16);
    CHECK(after == before);
    CHECK(late_before == 5);
    CHECK(late_after == late_before);
}

namespace {

// Counts callback executions and violations of the native thread-state contract.
struct AmbientChecks {
    unsigned faults{}, before{}, after{};
};

constexpr DWORD kIncomingError = 2001, kOutgoingError = 3001;
thread_local AmbientChecks *ambient_checks{};

// Verifies the incoming error and rounding state, then establishes distinct values for after observers.
unsigned __cdecl ambient_target() noexcept {
    if (GetLastError() != kIncomingError || WSAGetLastError() != static_cast<int>(kIncomingError) || (_mm_getcsr() & 0x6000U) != 0x4000U) {
        ++ambient_checks->faults;
    }

    _mm_setcsr((_mm_getcsr() & ~0x6000U) | 0x2000U);
    WSASetLastError(kOutgoingError);
    SetLastError(kOutgoingError);
    return 42;
}

// Each observer deliberately changes thread errors and SSE rounding mode to test dispatch restoration.
void disturb_ambient() noexcept {
    _mm_setcsr((_mm_getcsr() & ~0x6000U) | 0x6000U);
    WSASetLastError(4001);
    SetLastError(4001);
}

// Checks incoming errors, SSE rounding and carry, then disturbs ambient state for the next callback.
void FC_CDECL ambient_before(void *context, const FC_NativeFrame *frame, void *) noexcept {
    auto &checks = *static_cast<AmbientChecks *>(context);
    if (GetLastError() != kIncomingError || WSAGetLastError() != static_cast<int>(kIncomingError) || (_mm_getcsr() & 0x6000U) != 0x4000U ||
        (*frame->flags & 1) != 1) {
        ++checks.faults;
    }

    ++checks.before;
    disturb_ambient();
}

// Checks the callee's errors, rounding, carry and result, then disturbs state to test return restoration.
void FC_CDECL ambient_after(void *context, const FC_NativeFrame *frame, void *) noexcept {
    auto &checks = *static_cast<AmbientChecks *>(context);
    if (GetLastError() != kOutgoingError || WSAGetLastError() != static_cast<int>(kOutgoingError) || (_mm_getcsr() & 0x6000U) != 0x2000U ||
        (*frame->flags & 1) != 0 || *static_cast<const int *>(frame->result) != 42) {
        ++checks.faults;
    }

    ++checks.after;
    disturb_ambient();
}

// Appends a pointer-width immediate address for the fixture's x86 or x64 MOV instruction.
void append_address(std::vector<std::uint8_t> &code, std::uintptr_t address) {
    const auto *bytes = reinterpret_cast<const std::uint8_t *>(&address);
    code.insert(code.end(), bytes, bytes + sizeof(address));
}
} // namespace

TEST_CASE("Function observers preserve thread errors, SSE rounding and callee carry", "[native][hook]") {
    CodePage code;
    // The generated callee calls ambient_target and clears carry immediately before returning.
#if defined(_M_X64)
    std::vector<std::uint8_t> body{0x48, 0x83, 0xec, 0x28, 0x48, 0xb8};
#else
    std::vector<std::uint8_t> body{0xb8};
#endif
    append_address(body, reinterpret_cast<std::uintptr_t>(&ambient_target));
    body.insert(body.end(), {0xff, 0xd0});
#if defined(_M_X64)
    body.insert(body.end(), {0x48, 0x83, 0xc4, 0x28});
#endif
    body.insert(body.end(), {0xf8, 0xc3});
    code.put(5, body);

    // The independent caller sets entry carry and returns completed carry beside the native integer result.
#if defined(_M_X64)
    std::vector<std::uint8_t> caller{0x48, 0x83, 0xec, 0x28, 0x48, 0xb8};
#else
    std::vector<std::uint8_t> caller{0xb8};
#endif
    append_address(caller, code.address());
    // PUSHF/POP captures completed flags in EDX/RDX; x64 packs them above EAX, while x86 returns EDX:EAX.
    caller.insert(caller.end(), {0xf9, 0xff, 0xd0, 0x9c, 0x5a});
#if defined(_M_X64)
    caller.insert(caller.end(), {0x48, 0x83, 0xc4, 0x28, 0x48, 0xc1, 0xe2, 0x20, 0x89, 0xc0, 0x48, 0x09, 0xd0});
#endif
    caller.push_back(0xc3);
    code.put(128, caller);
    code.seal();

    AmbientChecks checks;
    HookRegistry hooks;
    SubmittedPlan plan{nullptr, {code.image()}, &hooks};
    for (unsigned index = 0; index != 2; ++index) {
        submit_hook(plan, 0, FC_HOOK_FUNCTION, &kNullary, &checks, nullptr, &ambient_before, &ambient_after);
    }
    install(plan);

    // Set the incoming rounding and error state only after hook preparation and installation have completed.
    ambient_checks = &checks;
    const auto previous_control = _mm_getcsr();
    _mm_setcsr((previous_control & ~0x6000U) | 0x4000U);
    WSASetLastError(kIncomingError);
    SetLastError(kIncomingError);

    // Capture final errors and control state before restoring the test thread's previous SSE control value.
    const auto returned = reinterpret_cast<std::uint64_t(__cdecl *)()>(code.address(128))();
    const auto final_error = GetLastError();
    const auto final_socket = WSAGetLastError();
    const auto final_control = _mm_getcsr();
    _mm_setcsr(previous_control);
    ambient_checks = nullptr;

    CHECK(static_cast<std::uint32_t>(returned) == 42);
    CHECK(((returned >> 32) & 1) == 0);
    CHECK(final_error == kOutgoingError);
    CHECK(final_socket == static_cast<int>(kOutgoingError));
    CHECK((final_control & 0x6000U) == 0x2000U);
    CHECK(checks.faults == 0);
    CHECK(checks.before == 2);
    CHECK(checks.after == 2);
}

TEST_CASE("Native callbacks retain the original caller through nested calls", "[native][hook][sdk]") {
    CodePage code;
    constexpr std::array<std::uint8_t, 6> body{0xb8, 0x2a, 0x00, 0x00, 0x00, 0xc3};
    code.put(5, body);
    std::array<std::uintptr_t, 2> expected{};

    // Independent machine callers give each invocation an exact return address outside the generated hook bridges.
    for (std::size_t index = 0; index < expected.size(); ++index) {
#if defined(_M_X64)
        std::vector<std::uint8_t> caller{0x48, 0x83, 0xec, 0x28, 0x48, 0xb8};
#else
        std::vector<std::uint8_t> caller{0xb8};
#endif
        const auto target = code.address();
        const auto *address = reinterpret_cast<const std::uint8_t *>(&target);
        caller.insert(caller.end(), address, address + sizeof(target));
        caller.insert(caller.end(), {0xff, 0xd0});
        const auto offset = static_cast<std::uint32_t>(128 * (index + 1));
        expected[index] = code.address(offset) + caller.size();
#if defined(_M_X64)
        caller.insert(caller.end(), {0x48, 0x83, 0xc4, 0x28});
#endif
        caller.push_back(0xc3);
        code.put(offset, caller);
    }
    code.seal();

    HookRegistry hooks;
    SubmittedPlan plan{nullptr, {code.image()}, &hooks};
    PlanView view{plan};
    using Call = int(__cdecl *)();
    std::vector<std::uintptr_t> entries, exits;
    bool nested_result{};
    view.sdk.observe<Call>(0, fc::before([&](const fc::NativeContext &context) noexcept {
                               entries.push_back(context.return_address());
                               if (entries.size() == 1) {
                                   nested_result = reinterpret_cast<Call>(code.address(256))() == 42;
                               }
                           }),
                           fc::after([&](const fc::NativeContext &context, const int &) noexcept {
                               exits.push_back(context.return_address());
                           }));
    REQUIRE_FALSE(plan.error());
    install(plan);

    REQUIRE(reinterpret_cast<Call>(code.address(128))() == 42);
    CHECK(nested_result);
    CHECK(entries == std::vector<std::uintptr_t>{expected[0], expected[1]});
    CHECK(exits == std::vector<std::uintptr_t>{expected[1], expected[0]});
    const auto query = fc::runtime::native_api()->return_address;
    REQUIRE(query != nullptr);
    CHECK(query() == 0);
}
