#include "runtime_contract.hpp"

#include <FusionCutter/SDK.hpp>

#include <winsock2.h>
#include <Windows.h>

#include <cstdint>
#include <utility>

namespace fc::fixtures::runtime {
namespace {

// Nested sections control the deliberate failure and bounded heterogeneous trace channel independently.
struct ObserverSettings {
    bool fail_prepare{};
    std::uint32_t trace_capacity = 16;
    std::uint32_t maximum_record_size = 64;
};

// One invocation-local expected result proves paired state is isolated across concurrent and nested dispatch.
struct ObservationState {
    std::int32_t expected_result;
};

// The larger trace shape follows a scalar activation record on the same channel.
struct DetailedTraceRecord {
    std::uint32_t kind{};
    std::uint32_t before_calls{};
    std::uint32_t after_calls{};
    std::uint32_t service_calls{};
};

// Observes the provider's mixed call, consumes its service, and publishes trace and live status results.
class ObserverHandler final {
  public:
    using Settings = ObserverSettings;

    ObserverHandler(const fc::CreateContext& context, const Settings& settings) noexcept
        : settings_(settings), logger_(context.logger()) {}

    // The observer joins the provider's physical site without acquiring ownership or changing its return value.
    void plan(fc::Plan& plan) {
        // Reserve the host-owned counters before any observer or status callback can see them.
        state_ = plan.require_mutable(fc::DataLocation<FC_SliceObserverState>{
            .rva = {FC_SLICE_OBSERVER_STATE_RVA}, .name = "ObserverState", .evidence = fc::expect(zero_state_)});

        // Both callbacks deliberately clobber ambient errors so the dispatcher must restore the authentic values.
        plan.observe<ObservationState>(fc::FunctionLocation<TypedSite>{.rva = {FC_SLICE_TYPED_SITE_RVA},
                                                                       .name = "MixedCallSite",
                                                                       .evidence = typed_site_evidence()},
#if defined(_M_IX86)
                                       fc::before([this](std::int32_t first, std::int32_t second, std::int32_t third,
                                                         ObservationState& invocation) noexcept {
                                           InterlockedIncrement(&state_->before_calls);
                                           invocation.expected_result = first + second + third + 15;
                                           SetLastError(0x1111);
                                           WSASetLastError(0x2222);
                                       }),
                                       fc::after([this](std::int32_t, std::int32_t, std::int32_t, std::int32_t result,
                                                        const ObservationState& invocation) noexcept {
                                           InterlockedIncrement(&state_->after_calls);
                                           if (result != invocation.expected_result) {
                                               InterlockedIncrement(&state_->state_mismatches);
                                           }
                                           SetLastError(0x3333);
                                           WSASetLastError(0x4444);
                                       })
#else
                                       fc::before([this](std::int32_t first, std::int32_t second, std::int32_t third,
                                                         std::int32_t fourth, std::int32_t fifth,
                                                         ObservationState& invocation) noexcept {
                                           InterlockedIncrement(&state_->before_calls);
                                           invocation.expected_result = first + second + third + fourth + fifth + 15;
                                           SetLastError(0x1111);
                                           WSASetLastError(0x2222);
                                       }),
                                       fc::after([this](std::int32_t, std::int32_t, std::int32_t, std::int32_t,
                                                        std::int32_t, std::int32_t result,
                                                        const ObservationState& invocation) noexcept {
                                           InterlockedIncrement(&state_->after_calls);
                                           if (result != invocation.expected_result) {
                                               InterlockedIncrement(&state_->state_mismatches);
                                           }
                                           SetLastError(0x3333);
                                           WSASetLastError(0x4444);
                                       })
#endif
        );
        // Optional routing exercises provider availability without creating another dependency edge.
        plan.bind<RuntimeServiceV1>("RuntimeProvider", *this, &ObserverHandler::connect);
        logger_.debug("Planned transparent observation and an optional provider binding");
    }

    // Required lookup proves dependency ordering; fallible trace allocation finishes before the Commit phase.
    fc::Result prepare(fc::PrepareContext& context) {
        // The table-driven failure stops this patch before native exposure and must prune only required consumers.
        if (settings_.fail_prepare) {
            context.logger().debug("Rejecting Prepare because the test setting requested a controlled failure");
            return std::unexpected(fc::Error{.message = "The configured runtime observer failure was requested",
                                             .operation = "Prepare observer fixture"});
        }
        // Immediate lookup consumes the provider only after activation ordered by dependencies has completed.
        const auto service = context.find_interface<RuntimeServiceV1>("RuntimeProvider");
        if (!service || !service->evaluate) {
            return std::unexpected(fc::Error{.message = "The required runtime provider interface is unavailable",
                                             .operation = "Find provider interface"});
        }
        immediate_result_ = service->evaluate(3);
        // Trace storage is the only substantial prepared resource and remains inactive until the Commit phase succeeds.
        auto trace = context.create_trace({.name = "RuntimeObserver",
                                           .capacity = settings_.trace_capacity,
                                           .max_record_size = settings_.maximum_record_size});
        if (!trace) {
            return std::unexpected(std::move(trace.error()));
        }
        trace_ = std::move(*trace);
        context.logger().debug("Prepared the required interface and trace channel");
        return {};
    }

    // Publish Prepare callback results only after the Commit phase and submit the channel's smaller record shape.
    void activate(fc::ActivateContext& context) noexcept {
        constexpr std::uint32_t activation_record = 1;
        state_->immediate_result = immediate_result_;
        InterlockedIncrement(&state_->service_calls);
        static_cast<void>(trace_.try_write(activation_record));
        context.logger().debug("Activated the runtime observer and submitted its first trace record");
    }

    // Record later runtime activity on the serialized pump and expose it through live status.
    void update(fc::UpdateContext& context) noexcept {
        // A larger record after the activation record proves one channel accepts heterogeneous payload sizes.
        const auto count = InterlockedIncrement(&state_->update_calls);
        const DetailedTraceRecord record{.kind = 2,
                                         .before_calls = static_cast<std::uint32_t>(state_->before_calls),
                                         .after_calls = static_cast<std::uint32_t>(state_->after_calls),
                                         .service_calls = static_cast<std::uint32_t>(state_->service_calls)};
        static_cast<void>(trace_.try_write(record));
        if (count == 1) {
            context.logger().debug("Submitted the first detailed runtime trace record");
        }
    }

    // Join paired callback health, interface delivery, and asynchronous trace accounting in runtime status.
    void write_status(fc::StatusWriter& output) const noexcept {
        if (state_ == nullptr) {
            return;
        }
        const auto health = trace_.health();
        (void)output.add("Before calls", state_->before_calls);
        (void)output.add("After calls", state_->after_calls);
        (void)output.add("State mismatches", state_->state_mismatches);
        (void)output.add("Immediate interface result", state_->immediate_result);
        (void)output.add("Bound interface result", state_->bound_result);
        (void)output.add("Trace records accepted", health.accepted);
        (void)output.add("Trace records written", health.written);
        (void)output.add("Trace records dropped", health.dropped);
    }

  private:
    // The required provider is already active, so delivery occurs during this consumer's publication boundary.
    void connect(RuntimeServiceV1 service) noexcept {
        if (service.evaluate) {
            const auto result = service.evaluate(4);
            state_->bound_result = result;
            InterlockedIncrement(&state_->service_calls);
            logger_.debug("Connected the optional provider interface with result {}", result);
        }
    }

    inline static constexpr FC_SliceObserverState zero_state_{};
    Settings settings_;
    fc::Logger logger_;
    // The Plan callback resolves this reviewed image state before any later lifecycle callback can use it.
    FC_SliceObserverState* state_{};
    std::int32_t immediate_result_{};
    fc::TraceChannel trace_;
};

// Activates before the provider so the optional binding must remain parked until provider publication.
class EarlyBindingHandler final {
  public:
    explicit EarlyBindingHandler(const fc::CreateContext& context) noexcept : logger_(context.logger()) {}

    // This patch sorts before the provider so the same optional route also waits for later provider activation.
    void plan(fc::Plan& plan) {
        plan.bind<RuntimeServiceV1>("RuntimeProvider", *this, &EarlyBindingHandler::connect);
        logger_.debug("Planned an interface binding before its provider is active");
    }

    // A nonzero result proves the provider connected after this earlier consumer was already active.
    void write_status(fc::StatusWriter& output) const noexcept {
        (void)output.add("Bound interface result", result_);
    }

  private:
    // This callback runs later on the serialized installation pump; the handler has no concurrent runtime paths.
    void connect(RuntimeServiceV1 service) noexcept {
        if (service.evaluate) {
            result_ = service.evaluate(6);
            logger_.debug("Connected the provider that activated after the early consumer");
        }
    }

    fc::Logger logger_;
    std::int32_t result_{};
};

// Makes pruning of required consumers visible without adding another native operation or failure source.
class ObserverConsumerHandler final {
  public:
    explicit ObserverConsumerHandler(const fc::CreateContext& context) noexcept : logger_(context.logger()) {}

    // This consumer has no native operations; it makes pruning visible after the deliberate observer failure.
    void activate(fc::ActivateContext&) noexcept {
        active_ = true;
        logger_.debug("Activated the dependent runtime observer consumer");
    }

    // The field differentiates successful activation from dependency pruning before this callback.
    void write_status(fc::StatusWriter& output) const noexcept {
        (void)output.add("Active", active_);
    }

  private:
    fc::Logger logger_;
    bool active_{};
};

// Builds the two named settings sections used by the real configuration path in every runtime row.
[[nodiscard]] fc::SettingsSchema<ObserverSettings> observer_settings() {
    // Named sections exercise the generated hierarchy without changing which typed settings reach the handler.
    fc::SettingsSchema<ObserverSettings> result;
    result.sections = {
        fc::section<ObserverSettings>(
            "Behavior", {fc::value("FailPrepare", &ObserverSettings::fail_prepare, false)
                             .description("Requests the runtime test's controlled Prepare callback failure")}),
        fc::section<ObserverSettings>(
            "Tracing", {fc::value("Capacity", &ObserverSettings::trace_capacity, std::uint32_t{16})
                            .range(std::uint32_t{1}, std::uint32_t{64})
                            .description("Maximum number of pending runtime fixture records"),
                        fc::value("MaximumRecordSize", &ObserverSettings::maximum_record_size, std::uint32_t{64})
                            .range(std::uint32_t{16}, std::uint32_t{256})
                            .description("Largest heterogeneous runtime fixture record in bytes")}),
    };
    return result;
}

// All startup observer patches use the same private Game image support tuple.
template <class Handler> [[nodiscard]] fc::Support game_support() {
    return fc::support<Handler>({.layouts = {fc::TargetLayout::GOGRetail, fc::TargetLayout::ClassicCollection},
                                 .roles = fc::HostRole::All,
                                 .image = fc::TargetImage::Game});
}

} // namespace

// The dependent consumer turns the controlled observer failure into an observable pruning and continuation case.
fc::Plugin build_observer_plugin() {
    return fc::plugin({
        .id = "RuntimeObserverSlice",
        .version = "1.0.0",
        .patches =
            {
                fc::patch<EarlyBindingHandler>({.id = "RuntimeEarlyBinding",
                                                .name = "Runtime early binding",
                                                .enabled = false,
                                                .description = "Consumes the provider when it activates later",
                                                .supports = {game_support<EarlyBindingHandler>()}}),
                fc::patch<ObserverHandler>({.id = "RuntimeObserver",
                                            .name = "Runtime observer",
                                            .enabled = true,
                                            .description = "Observes the mixed call and consumes its provider",
                                            .settings = observer_settings(),
                                            .depends_on = {"RuntimeProvider"},
                                            .supports = {game_support<ObserverHandler>()}}),
                fc::patch<ObserverConsumerHandler>({.id = "RuntimeObserverConsumer",
                                                    .name = "Runtime observer consumer",
                                                    .enabled = true,
                                                    .configurable = false,
                                                    .description = "Depends on the observer for pruning coverage",
                                                    .depends_on = {"RuntimeObserver"},
                                                    .supports = {game_support<ObserverConsumerHandler>()}}),
            },
    });
}

} // namespace fc::fixtures::runtime
