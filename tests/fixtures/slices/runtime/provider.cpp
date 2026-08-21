#include "runtime_contract.hpp"

#include <FusionCutter/SDK.hpp>

#include <Windows.h>

#include <cstdint>
#include <optional>

namespace fc::fixtures::runtime {
namespace {

// Owns both startup hook sites and publishes the copied service consumed by independent plugins.
class ProviderHandler final {
  public:
    explicit ProviderHandler(const fc::CreateContext& context) noexcept : logger_(context.logger()) {}

    // Own both startup sites and retain the typed original for work after the Plan callback has returned.
    void plan(fc::Plan& plan) {
        // Reserve the host counters for callbacks and status throughout the patch's installed lifetime.
        state_ = plan.require_mutable(fc::DataLocation<FC_SliceProviderState>{
            .rva = {FC_SLICE_PROVIDER_STATE_RVA}, .name = "ProviderState", .evidence = fc::expect(zero_state_)});

        // The typed owner records dispatch, calls the physical original, and changes only the returned value.
        original_ = plan.hook(fc::FunctionLocation<TypedSite>{.rva = {FC_SLICE_TYPED_SITE_RVA},
                                                              .name = "MixedCallSite",
                                                              .evidence = typed_site_evidence()},
#if defined(_M_IX86)
                              [this](fc::Original<TypedSite> original, std::int32_t first, std::int32_t second,
                                     std::int32_t third) noexcept {
                                  InterlockedIncrement(&state_->owner_calls);
                                  return original(first, second, third) + 5;
                              }
#else
            [this](fc::Original<TypedSite> original, std::int32_t first, std::int32_t second, std::int32_t third,
                   std::int32_t fourth, std::int32_t fifth) noexcept {
                InterlockedIncrement(&state_->owner_calls);
                return original(first, second, third, fourth, fifth) + 5;
            }
#endif
        );
        // The instruction owner records entry while leaving the captured CPU context unchanged.
        plan.hook(fc::CodeLocation{.rva = {FC_SLICE_INSTRUCTION_SITE_RVA},
                                   .name = "InstructionSite",
                                   .evidence = instruction_site_evidence()},
                  [this](fc::CpuContext&) noexcept {
                      InterlockedIncrement(&state_->instruction_calls);
                  });
        logger_.debug("Planned ownership of the mixed native call and instruction sites");
    }

    // Calling Original during activation proves the retained handle is bound outside generated hook dispatch.
    void activate(fc::ActivateContext& context) noexcept {
#if defined(_M_IX86)
        state_->activate_original_result = (*original_)(1, 1, 1);
#else
        state_->activate_original_result = (*original_)(1, 1, 1, 1, 1);
#endif
        context.logger().debug("Retained Original returned {} during activation", state_->activate_original_result);
    }

    // The copied interface contains an SDK thunk rather than a C++ object reference crossing plugin DLLs.
    void query_interface(fc::InterfaceQuery& query) noexcept {
        query.provide(RuntimeServiceV1{fc::interface_function<&ProviderHandler::evaluate>(*this)});
    }

    // Count serialized pump delivery while keeping repetitive diagnostics quiet.
    void update(fc::UpdateContext& context) noexcept {
        // Only the first pump event is diagnostically useful; later events remain visible through live status.
        const auto count = InterlockedIncrement(&state_->update_calls);
        if (count == 1) {
            context.logger().debug("The provider received its first serialized update");
        }
    }

    // Expose native dispatch and retained service use without revealing handler storage.
    void write_status(fc::StatusWriter& output) const noexcept {
        if (state_ == nullptr) {
            return;
        }
        (void)output.add("Owner calls", state_->owner_calls);
        (void)output.add("Instruction calls", state_->instruction_calls);
        (void)output.add("Service calls", state_->service_calls);
        (void)output.add("Retained original result", state_->activate_original_result);
    }

  private:
    // A conspicuous transform lets each consumer prove it received and invoked the copied interface.
    std::int32_t evaluate(std::int32_t value) noexcept {
        InterlockedIncrement(&state_->service_calls);
        return value + 100;
    }

    inline static constexpr FC_SliceProviderState zero_state_{};
    fc::Logger logger_;
    // The Plan callback resolves this reviewed image state before any later lifecycle callback can use it.
    FC_SliceProviderState* state_{};
    // A successful plan binds this handle before the Activate callback and keeps it callable while installed.
    std::optional<fc::Original<TypedSite>> original_;
};

// Owns the peer call only after the x86 late image profile has recognized and pinned the DLL.
class LateOwnerHandler final {
  public:
    explicit LateOwnerHandler(const fc::CreateContext& context) noexcept : logger_(context.logger()) {}

    // The late pass owns an absent startup site only after the reviewed peer image has been recognized.
    void plan(fc::Plan& plan) {
        plan.hook(fc::FunctionLocation<LateSite>{.rva = {FC_SLICE_LATE_SITE_RVA},
                                                 .name = "LateCallSite",
                                                 .evidence = late_site_evidence()},
                  [this](fc::Original<LateSite> original, std::int32_t value) noexcept {
                      InterlockedIncrement(&owner_calls_);
                      return original(value) + 7;
                  });
        logger_.debug("Planned ownership of the newly recognized peer call site");
    }

    // The late image counts the physical original; this status field identifies owner dispatch.
    void write_status(fc::StatusWriter& output) const noexcept {
        (void)output.add("Owner calls", owner_calls_);
    }

  private:
    fc::Logger logger_;
    volatile LONG owner_calls_{};
};

// The startup handler is shared by the private x86 and x64 server targets.
[[nodiscard]] fc::Support startup_support() {
    return fc::support<ProviderHandler>({.layouts = {fc::TargetLayout::GOGRetail, fc::TargetLayout::ClassicCollection},
                                         .roles = fc::HostRole::All,
                                         .image = fc::TargetImage::Game});
}

// The late owner is valid only for the one reviewed GOG server peer tuple.
[[nodiscard]] fc::Support late_support() {
    return fc::support<LateOwnerHandler>({.layouts = {fc::TargetLayout::GOGRetail},
                                          .roles = fc::HostRole::Server,
                                          .image = fc::TargetImage::GalaxyPeer});
}

} // namespace

// The startup provider softly selects both late participants so neither needs a separate user-facing toggle.
fc::Plugin build_provider_plugin() {
    return fc::plugin({
        .id = "RuntimeProviderSlice",
        .version = "1.0.0",
        .patches =
            {
                fc::patch<ProviderHandler>({.id = "RuntimeProvider",
                                            .name = "Runtime provider",
                                            .enabled = true,
                                            .description = "Owns startup hook sites and exposes a copied service",
                                            .includes = {"RuntimeLateOwner", "RuntimeLateParticipant"},
                                            .supports = {startup_support()}}),
                fc::patch<LateOwnerHandler>({.id = "RuntimeLateOwner",
                                             .name = "Runtime late owner",
                                             .enabled = false,
                                             .configurable = false,
                                             .description = "Owns the peer call site when its image arrives",
                                             .supports = {late_support()}}),
            },
    });
}

} // namespace fc::fixtures::runtime
