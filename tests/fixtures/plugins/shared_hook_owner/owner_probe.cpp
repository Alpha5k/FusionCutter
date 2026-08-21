#include "../shared_hook_probe.hpp"

#include <FusionCutter/SDK.hpp>

#include <cstdint>
#include <limits>
#include <optional>

#if defined(_M_IX86)
#pragma comment(linker, "/export:FcHookProbeDirectTarget=_FcHookProbeDirectTarget")
#pragma comment(linker, "/export:FcHookProbeFastcallOwnerThunk=_FcHookProbeFastcallOwnerThunk")
#pragma comment(linker, "/export:FcHookProbeGetFastcallBuilder=_FcHookProbeGetFastcallBuilder")
#pragma comment(linker, "/export:FcHookProbeGetFloatBuilder=_FcHookProbeGetFloatBuilder")
#pragma comment(linker, "/export:FcHookProbeGetMixedBuilder=_FcHookProbeGetMixedBuilder")
#pragma comment(linker, "/export:FcHookProbeGetOwnerBuilder=_FcHookProbeGetOwnerBuilder")
#pragma comment(linker, "/export:FcHookProbeGetRecordBuilder=_FcHookProbeGetRecordBuilder")
#pragma comment(linker, "/export:FcHookProbeGetRecordArgumentBuilder=_FcHookProbeGetRecordArgumentBuilder")
#pragma comment(linker, "/export:FcHookProbeGetStdcallBuilder=_FcHookProbeGetStdcallBuilder")
#pragma comment(linker, "/export:FcHookProbeGetThiscallBuilder=_FcHookProbeGetThiscallBuilder")
#pragma comment(linker, "/export:FcHookProbeGetVoidBuilder=_FcHookProbeGetVoidBuilder")
#pragma comment(linker, "/export:FcHookProbeInvokeFastcall=_FcHookProbeInvokeFastcall")
#pragma comment(linker, "/export:FcHookProbeInvokeFloat=_FcHookProbeInvokeFloat")
#pragma comment(linker, "/export:FcHookProbeInvokeMixed=_FcHookProbeInvokeMixed")
#pragma comment(linker, "/export:FcHookProbeInvokeRetainedMixedOriginal=_FcHookProbeInvokeRetainedMixedOriginal")
#pragma comment(linker, "/export:FcHookProbeInvokeRecord=_FcHookProbeInvokeRecord")
#pragma comment(linker, "/export:FcHookProbeInvokeRecordArgument=_FcHookProbeInvokeRecordArgument")
#pragma comment(linker, "/export:FcHookProbeInvokeStdcall=_FcHookProbeInvokeStdcall")
#pragma comment(linker, "/export:FcHookProbeInvokeThiscall=_FcHookProbeInvokeThiscall")
#pragma comment(linker, "/export:FcHookProbeMixedOwnerThunk=_FcHookProbeMixedOwnerThunk")
#pragma comment(linker, "/export:FcHookProbeFloatOwnerThunk=_FcHookProbeFloatOwnerThunk")
#pragma comment(linker, "/export:FcHookProbeOwnerThunk=_FcHookProbeOwnerThunk")
#pragma comment(linker, "/export:FcHookProbeRecordOwnerThunk=_FcHookProbeRecordOwnerThunk")
#pragma comment(linker, "/export:FcHookProbeRecordArgumentOwnerThunk=_FcHookProbeRecordArgumentOwnerThunk")
#pragma comment(linker, "/export:FcHookProbeStdcallOwnerThunk=_FcHookProbeStdcallOwnerThunk")
#pragma comment(linker, "/export:FcHookProbeThiscallOwnerThunk=_FcHookProbeThiscallOwnerThunk")
#endif

namespace {

#if defined(_M_IX86)
using MixedCall = fc::NativeCall<std::int32_t(std::int32_t, std::int32_t, std::int32_t),
                                 fc::abi::x86<fc::abi::args<fc::abi::esi, fc::abi::edi, fc::abi::stack<8>>,
                                              fc::abi::result<fc::abi::ebx>, fc::abi::caller_cleanup>>;
#else
using MixedCall = fc::NativeCall<
    std::int32_t(std::int32_t, std::int32_t, std::int32_t, std::int32_t, std::int32_t),
    fc::abi::x64<fc::abi::args<fc::abi::r10, fc::abi::r11, fc::abi::r12, fc::abi::r13, fc::abi::stack<16>>,
                 fc::abi::result<fc::abi::r11>>>;
#endif

using VoidCall = void(FC_CALL*)(std::int32_t value) noexcept;

#if defined(_M_IX86)
using FloatCall =
    fc::NativeCall<double(double, float), fc::abi::x86<fc::abi::args<fc::abi::xmm5, fc::abi::stack<8>>,
                                                       fc::abi::result<fc::abi::xmm6>, fc::abi::caller_cleanup>>;
#else
using FloatCall =
    fc::NativeCall<double(double, float),
                   fc::abi::x64<fc::abi::args<fc::abi::xmm9, fc::abi::xmm10>, fc::abi::result<fc::abi::xmm11>>>;
#endif

#if defined(_M_IX86)
using RecordCall = fc::NativeCall<FcHookProbeRecord(std::int32_t, std::int32_t),
                                  fc::abi::x86<fc::abi::args<fc::abi::edi, fc::abi::stack<4>>,
                                               fc::abi::hidden_result<fc::abi::esi>, fc::abi::caller_cleanup>>;
using RecordArgumentCall = fc::NativeCall<std::int32_t(FcHookProbeRecord, std::int32_t),
                                          fc::abi::x86<fc::abi::args<fc::abi::stack<8>, fc::abi::edi>,
                                                       fc::abi::result<fc::abi::ebx>, fc::abi::caller_cleanup>>;
#else
using RecordCall =
    fc::NativeCall<FcHookProbeRecord(std::int32_t, std::int32_t),
                   fc::abi::x64<fc::abi::args<fc::abi::r10, fc::abi::stack<8>>, fc::abi::hidden_result<fc::abi::r14>>>;
using RecordArgumentCall =
    fc::NativeCall<std::int32_t(FcHookProbeRecord, std::int32_t),
                   fc::abi::x64<fc::abi::args<fc::abi::stack<24>, fc::abi::r15>, fc::abi::result<fc::abi::r10>>>;
#endif

// Retaining the public handle outside the owner callback proves it remains callable after the Plan callback.
std::optional<fc::Original<MixedCall>> retained_mixed_original;

#if defined(_M_IX86)
using StdcallCall = std::int32_t(__stdcall*)(std::int32_t, std::int32_t) noexcept;
using FastcallCall = std::int32_t(__fastcall*)(std::int32_t, std::int32_t) noexcept;

struct ThiscallObject {
    std::int32_t base{};
};

using ThiscallCall = std::int32_t(__thiscall*)(ThiscallObject*, std::int32_t) noexcept;
#endif

// This SDK patch supplies the prototype's Original binding and owner thunk owned by the patch plan.
class SdkOwnerHandler final {
  public:
    void plan(fc::Plan& plan) {
        // The ordinary owner proves the public SDK supplies an Original that can be called through its trampoline.
        plan.hook_entry_at<FcHookProbeCall>(
            fc::Rva{0}, [](fc::Original<FcHookProbeCall> original, std::int32_t left, std::int32_t right) noexcept {
                return original(left, right) + 13;
            });
        // The explicit owner is retained so reverse adaptation is exercised both inside and outside dispatch.
        retained_mixed_original =
            plan.hook_entry_at<MixedCall>(fc::Rva{8}, [](fc::Original<MixedCall> original, auto... arguments) noexcept {
                return original(arguments...) + 17;
            });
    }
};

// Registers exactly one real patch through the public SDK so the executable can capture its copied ABI requests.
fc::Plugin build_probe_plugin() {
    return fc::plugin({
        .id = "SharedHookOwnerProbe",
        .patches =
            {
                fc::patch<SdkOwnerHandler>({
                    .id = "SdkOwner",
                    .name = "SDK owner probe",
                    .supports =
                        {
                            fc::support({
                                .layouts = {fc::TargetLayout::GameSpyRetail},
                                .roles = fc::HostRole::Client,
                                .image = fc::TargetImage::Game,
                            }),
                        },
                }),
            },
    });
}

} // namespace

FC_EXPORT_PLUGIN(build_probe_plugin)

// Exposes the real stateless SDK builder without sharing C++ adapter objects with the test executable.
extern "C" __declspec(dllexport) void FC_CALL FcHookProbeGetOwnerBuilder(FC_HookBuilder* output) noexcept {
    if (output != nullptr) {
        *output = fc::detail::typed_hook_builder<FcHookProbeCall>();
    }
}

extern "C" __declspec(dllexport) void FC_CALL FcHookProbeGetMixedBuilder(FC_HookBuilder* output) noexcept {
    if (output != nullptr) {
        *output = fc::detail::typed_hook_builder<MixedCall>();
    }
}

extern "C" __declspec(dllexport) void FC_CALL FcHookProbeGetVoidBuilder(FC_HookBuilder* output) noexcept {
    if (output != nullptr) {
        *output = fc::detail::typed_hook_builder<VoidCall>();
    }
}

extern "C" __declspec(dllexport) void FC_CALL FcHookProbeGetRecordBuilder(FC_HookBuilder* output) noexcept {
    if (output != nullptr) {
        *output = fc::detail::typed_hook_builder<RecordCall>();
    }
}

extern "C" __declspec(dllexport) void FC_CALL FcHookProbeGetRecordArgumentBuilder(FC_HookBuilder* output) noexcept {
    if (output != nullptr) {
        *output = fc::detail::typed_hook_builder<RecordArgumentCall>();
    }
}

extern "C" __declspec(dllexport) void FC_CALL FcHookProbeGetFloatBuilder(FC_HookBuilder* output) noexcept {
    if (output != nullptr) {
        *output = fc::detail::typed_hook_builder<FloatCall>();
    }
}

#if defined(_M_IX86)
// x86 keeps native compiler ABI conventions distinct, so each SDK spelling receives a generated entry.
extern "C" __declspec(dllexport) void FC_CALL FcHookProbeGetStdcallBuilder(FC_HookBuilder* output) noexcept {
    if (output != nullptr) {
        *output = fc::detail::typed_hook_builder<StdcallCall>();
    }
}

extern "C" __declspec(dllexport) void FC_CALL FcHookProbeGetFastcallBuilder(FC_HookBuilder* output) noexcept {
    if (output != nullptr) {
        *output = fc::detail::typed_hook_builder<FastcallCall>();
    }
}

extern "C" __declspec(dllexport) void FC_CALL FcHookProbeGetThiscallBuilder(FC_HookBuilder* output) noexcept {
    if (output != nullptr) {
        *output = fc::detail::typed_hook_builder<ThiscallCall>();
    }
}
#endif

// Models a modifying plugin that calls the physical original and then changes only the returned value.
extern "C" __declspec(dllexport) std::int32_t FC_CALL FcHookProbeOwnerThunk(void* context, std::int32_t left,
                                                                            std::int32_t right) noexcept {
    auto& owner = *static_cast<FcHookProbeOwnerContext*>(context);
    InterlockedIncrement(&owner.calls);
    const auto original = reinterpret_cast<FcHookProbeCall>(owner.original);
    const auto direct = original(left, right) + owner.adjustment;
    // One re-entry at the same generated site proves each invocation has its own paired state.
    static thread_local bool nested = false;
    if (owner.nested_entry == 0 || nested) {
        return direct;
    }
    nested = true;
    const auto nested_result = reinterpret_cast<FcHookProbeCall>(owner.nested_entry)(1, 2);
    nested = false;
    return direct + nested_result;
}

#if defined(_M_IX86)
extern "C" __declspec(dllexport) std::int32_t
    FC_CALL FcHookProbeMixedOwnerThunk(void*, std::int32_t first, std::int32_t second, std::int32_t third) noexcept {
    return first + second + third;
}
#else
extern "C" __declspec(dllexport) std::int32_t
    FC_CALL FcHookProbeMixedOwnerThunk(void*, std::int32_t first, std::int32_t second, std::int32_t third,
                                       std::int32_t fourth, std::int32_t fifth) noexcept {
    return first + second + third + fourth + fifth;
}
#endif

// Exercises the SDK's callable reverse adapter; the physical homes deliberately do not match a compiler convention.
extern "C" __declspec(dllexport) std::int32_t FC_CALL FcHookProbeInvokeMixed(std::uintptr_t entry, std::int32_t first,
                                                                             std::int32_t second, std::int32_t third,
                                                                             std::int32_t fourth,
                                                                             std::int32_t fifth) noexcept {
#if defined(_M_IX86)
    (void)fourth;
    (void)fifth;
#endif
    return fc::detail::NativeCallable<MixedCall>{entry}(first, second, third
#if defined(_M_X64)
                                                        ,
                                                        fourth, fifth
#endif
    );
}

// Calls the Original returned by the Plan callback from retained plugin state outside hook dispatch.
extern "C" __declspec(dllexport) std::int32_t FC_CALL FcHookProbeInvokeRetainedMixedOriginal(
    std::int32_t first, std::int32_t second, std::int32_t third, std::int32_t fourth, std::int32_t fifth) noexcept {
    if (!retained_mixed_original || !*retained_mixed_original) {
        return std::numeric_limits<std::int32_t>::min();
    }
#if defined(_M_IX86)
    (void)fourth;
    (void)fifth;
#endif
    return (*retained_mixed_original)(first, second, third
#if defined(_M_X64)
                                      ,
                                      fourth, fifth
#endif
    );
}

// Record and floating probes cover ABI paths whose argument or result cannot be represented by integer registers
// alone; each invoke export crosses the SDK reverse adapter and each owner export receives its forward conversion.
extern "C" __declspec(dllexport) FcHookProbeRecord FC_CALL FcHookProbeInvokeRecord(std::uintptr_t entry,
                                                                                   std::int32_t left,
                                                                                   std::int32_t right) noexcept {
    return fc::detail::NativeCallable<RecordCall>{entry}(left, right);
}

extern "C" __declspec(dllexport) FcHookProbeRecord FC_CALL FcHookProbeRecordOwnerThunk(void*, std::int32_t left,
                                                                                       std::int32_t right) noexcept {
    return {left, right, left + right};
}

extern "C" __declspec(dllexport) std::int32_t FC_CALL
FcHookProbeInvokeRecordArgument(std::uintptr_t entry, FcHookProbeRecord record, std::int32_t adjustment) noexcept {
    return fc::detail::NativeCallable<RecordArgumentCall>{entry}(record, adjustment);
}

extern "C" __declspec(dllexport) std::int32_t FC_CALL
FcHookProbeRecordArgumentOwnerThunk(void*, FcHookProbeRecord record, std::int32_t adjustment) noexcept {
    return record.total + adjustment;
}

extern "C" __declspec(dllexport) double FC_CALL FcHookProbeInvokeFloat(std::uintptr_t entry, double left,
                                                                       float right) noexcept {
    return fc::detail::NativeCallable<FloatCall>{entry}(left, right);
}

extern "C" __declspec(dllexport) double FC_CALL FcHookProbeFloatOwnerThunk(void*, double left, float right) noexcept {
    return left + right;
}

// A stable native target lets the executable prove both placement at the global entry and placement at one call site.
extern "C" __declspec(noinline) __declspec(dllexport) std::int32_t FC_CALL
FcHookProbeDirectTarget(std::int32_t left, std::int32_t right) noexcept {
    SetLastError(0x7101);
    WSASetLastError(0x7102);
    return left + right;
}

#if defined(_M_IX86)
// Native x86 convention probes use compiler calls at the test boundary so the generated entry receives authentic
// stdcall, fastcall, and thiscall register/cleanup behavior.
extern "C" __declspec(dllexport) std::int32_t FC_CALL FcHookProbeStdcallOwnerThunk(void*, std::int32_t left,
                                                                                   std::int32_t right) noexcept {
    return left * 10 + right;
}

extern "C" __declspec(dllexport) std::int32_t FC_CALL FcHookProbeFastcallOwnerThunk(void*, std::int32_t left,
                                                                                    std::int32_t right) noexcept {
    return left * 10 + right;
}

extern "C" __declspec(dllexport) std::int32_t FC_CALL FcHookProbeThiscallOwnerThunk(void*, ThiscallObject* object,
                                                                                    std::int32_t value) noexcept {
    return object->base + value;
}

extern "C" __declspec(dllexport) std::int32_t FC_CALL FcHookProbeInvokeStdcall(std::uintptr_t entry, std::int32_t left,
                                                                               std::int32_t right) noexcept {
    return reinterpret_cast<StdcallCall>(entry)(left, right);
}

extern "C" __declspec(dllexport) std::int32_t FC_CALL FcHookProbeInvokeFastcall(std::uintptr_t entry, std::int32_t left,
                                                                                std::int32_t right) noexcept {
    return reinterpret_cast<FastcallCall>(entry)(left, right);
}

extern "C" __declspec(dllexport) std::int32_t FC_CALL FcHookProbeInvokeThiscall(std::uintptr_t entry, std::int32_t base,
                                                                                std::int32_t value) noexcept {
    ThiscallObject object{base};
    return reinterpret_cast<ThiscallCall>(entry)(&object, value);
}
#endif
