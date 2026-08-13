#include "../../../../src/patches/networking/network_pipeline/pipeline.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

namespace network_pipeline = fusioncutter::patches::network_pipeline;

namespace {

struct Observation {
    std::uint32_t calls{};
    std::uint32_t first{};
    std::uint32_t second{};
    std::uint32_t stack{};
};

struct CallState {
    std::uint32_t before_esp{};
    std::uint32_t after_esp{};
    std::uint32_t ebx{};
    std::uint32_t esi{};
    std::uint32_t edi{};
    std::uint32_t result{};
};

struct TestTransport {
    std::uint32_t transmits{};
    std::uint32_t groups_started{};
    std::uint32_t groups_ended{};
};

Observation gFinal;
Observation gGroup;

int __cdecl observe_final(std::uint32_t destination, std::uint32_t bytes, std::uint32_t length) noexcept {
    gFinal = {gFinal.calls + 1, destination, bytes, length};
    return 77;
}

void __cdecl observe_group(std::uint32_t destination, std::uint32_t argument, std::uint32_t group) noexcept {
    gGroup = {gGroup.calls + 1, destination, argument, group};
}

__declspec(naked) int synthetic_final_send() {
    __asm {
        push dword ptr [esp + 4]
        push edx
        push ecx
        call observe_final
        add  esp, 12
        ret
    }
}

__declspec(naked) void synthetic_group_send() {
    __asm {
        push dword ptr [esp + 4]
        push edx
        push ecx
        call observe_group
        add  esp, 12
        ret
    }
}

__declspec(naked) void invoke_game_send(void*, std::uint32_t, std::uint32_t, std::uint32_t, CallState*) {
    __asm {
        push ebp
        mov  ebp, esp
        push ebx
        push esi
        push edi
        mov  ebx, 0x11223344
        mov  esi, 0x55667788
        mov  edi, 0x99aabbcc
        mov  ecx, dword ptr [ebp + 12]
        mov  edx, dword ptr [ebp + 16]
        push dword ptr [ebp + 20]
        mov  eax, dword ptr [ebp + 24]
        mov  dword ptr [eax], esp
        call dword ptr [ebp + 8]
        mov  ecx, dword ptr [ebp + 24]
        mov  dword ptr [ecx + 4], esp
        mov  dword ptr [ecx + 8], ebx
        mov  dword ptr [ecx + 12], esi
        mov  dword ptr [ecx + 16], edi
        mov  dword ptr [ecx + 20], eax
        add  esp, 4
        pop  edi
        pop  esi
        pop  ebx
        mov  esp, ebp
        pop  ebp
        ret
    }
}

network_pipeline::TransportCallbacks callbacks_for(TestTransport& transport) noexcept {
    return {
        .context = &transport,
        .before_receive = [](void*) noexcept {},
        .after_receive = [](void*) noexcept {},
        .native_transmit =
            [](void* context, int) noexcept {
                ++static_cast<TestTransport*>(context)->transmits;
            },
        .begin_group =
            [](void* context, int destination) noexcept {
                ++static_cast<TestTransport*>(context)->groups_started;
                return destination;
            },
        .end_group =
            [](void* context, int) noexcept {
                ++static_cast<TestTransport*>(context)->groups_ended;
            },
        .send =
            [](void*, int, int, std::span<const std::uint8_t>) noexcept {
                return network_pipeline::NativeSendResult{};
            },
        .intake = [](void*, void*) noexcept {},
        .disconnect = [](void*, int) noexcept {},
        .disconnect_complete = [](void*, int) noexcept {},
        .reset = [](void*, std::uint8_t) noexcept {},
    };
}

[[nodiscard]] bool preserves_call_frame(const CallState& state) noexcept {
    return state.before_esp == state.after_esp && state.ebx == 0x11223344 && state.esi == 0x55667788 &&
           state.edi == 0x99AABBCC;
}

} // namespace

TEST_CASE("Network Pipeline preserves the game's nonstandard x86 send ABI") {
    TestTransport transport;
    const auto callbacks = callbacks_for(transport);
    network_pipeline::configure_hooks_for_test(
        callbacks, {reinterpret_cast<void*>(&synthetic_final_send), reinterpret_cast<void*>(&synthetic_group_send)});

    CallState final_state{};
    invoke_game_send(network_pipeline::hook_for_test(network_pipeline::HookPoint::FinalSend), 17, 0x50607080, 1009,
                     &final_state);
    CHECK(preserves_call_frame(final_state));
    CHECK(final_state.result == 77);
    CHECK(gFinal.calls == 1);
    CHECK(gFinal.first == 17);
    CHECK(gFinal.second == 0x50607080);
    CHECK(gFinal.stack == 1009);

    CallState group_state{};
    invoke_game_send(network_pipeline::hook_for_test(network_pipeline::HookPoint::GroupSend), 23, 0x60708090,
                     0xA0B0C0D0, &group_state);
    CHECK(preserves_call_frame(group_state));
    CHECK(gGroup.calls == 1);
    CHECK(gGroup.first == 23);
    CHECK(gGroup.second == 0x60708090);
    CHECK(gGroup.stack == 0xA0B0C0D0);
    CHECK(transport.transmits == 1);
    CHECK(transport.groups_started == 1);
    CHECK(transport.groups_ended == 1);
}
