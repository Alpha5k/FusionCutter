#include "runtime_contract.hpp"

#include <FusionCutter/SDK.hpp>

#include <Windows.h>

#include <cstdint>

namespace fc::fixtures::runtime {
namespace {

// One invocation-local expectation proves the observer joins the installed late site without changing ownership.
struct LateObservationState {
    std::int32_t expected_result;
};

// Joins the late owner's site and consumes the already active startup provider through an optional binding.
class LateParticipantHandler final {
  public:
    explicit LateParticipantHandler(const fc::CreateContext& context) noexcept : logger_(context.logger()) {}

    // This observer joins the provider's installed site through the late pass and keeps ownership unchanged.
    void plan(fc::Plan& plan) {
        plan.observe<LateObservationState>(
            fc::FunctionLocation<LateSite>{
                .rva = {FC_SLICE_LATE_SITE_RVA}, .name = "LateCallSite", .evidence = late_site_evidence()},
            fc::before([this](std::int32_t value, LateObservationState& invocation) noexcept {
                InterlockedIncrement(&before_calls_);
                invocation.expected_result = value + 7;
            }),
            fc::after([this](std::int32_t, std::int32_t result, const LateObservationState& invocation) noexcept {
                InterlockedIncrement(&after_calls_);
                if (result != invocation.expected_result) {
                    InterlockedIncrement(&mismatches_);
                }
            }));
        // The already active provider fulfills this route before the late hook snapshot is published.
        plan.bind<RuntimeServiceV1>("RuntimeProvider", *this, &LateParticipantHandler::connect);
        logger_.debug("Planned late observation and an optional binding to the provider from startup");
    }

    // Plugin-owned counters show the joining snapshot ran without unsafe writes in the late image plan.
    void write_status(fc::StatusWriter& output) const noexcept {
        (void)output.add("Before calls", before_calls_);
        (void)output.add("After calls", after_calls_);
        (void)output.add("State mismatches", mismatches_);
        (void)output.add("Bound interface result", bound_result_);
    }

  private:
    // The active provider completes this late patch's optional binding during publication.
    void connect(RuntimeServiceV1 service) noexcept {
        if (service.evaluate) {
            bound_result_ = service.evaluate(5);
            logger_.debug("Connected the startup provider while publishing the late participant");
        }
    }

    fc::Logger logger_;
    volatile LONG before_calls_{};
    volatile LONG after_calls_{};
    volatile LONG mismatches_{};
    LONG bound_result_{};
};

// The participant waits only on the reviewed GOG server peer image.
[[nodiscard]] fc::Support late_support() {
    return fc::support<LateParticipantHandler>({.layouts = {fc::TargetLayout::GOGRetail},
                                                .roles = fc::HostRole::Server,
                                                .image = fc::TargetImage::GalaxyPeer});
}

} // namespace

// Selection remains soft: the provider includes this dormant patch only for a compatible waiting image.
fc::Plugin build_late_participant_plugin() {
    return fc::plugin({
        .id = "RuntimeLateParticipantSlice",
        .version = "1.0.0",
        .patches =
            {
                fc::patch<LateParticipantHandler>({.id = "RuntimeLateParticipant",
                                                   .name = "Runtime late participant",
                                                   .enabled = false,
                                                   .configurable = false,
                                                   .description = "Joins an installed peer site after image arrival",
                                                   .supports = {late_support()}}),
            },
    });
}

} // namespace fc::fixtures::runtime
