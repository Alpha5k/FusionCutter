#include "hero_diagnostics.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cstddef>
#include <cstring>
#include <string_view>
#include <utility>

namespace fusioncutter::patches::hero_diagnostics {
namespace {

constexpr std::size_t kStateOffset = 0x180;
constexpr std::size_t kPreviousStateOffset = 0x184;
constexpr std::size_t kStateTimeOffset = 0x188;
constexpr std::size_t kInputTimeOffset = 0x130;
constexpr std::size_t kPositionOffset = 0x120;
constexpr std::size_t kVelocityOffset = 0x4DC;
constexpr std::size_t kInputQueueOffset = 0x134;
constexpr std::size_t kInputQueueCountOffset = 0x40;
constexpr std::size_t kInputQueueHeadOffset = 0x41;
constexpr std::size_t kInputQueueDownOffset = 0x42;
constexpr std::size_t kRemoteMoveStride = 0x18;
constexpr std::size_t kPrimaryTriggerOffset = 0x38;
constexpr std::size_t kSecondaryTriggerOffset = 0x3C;
constexpr std::size_t kJumpTriggerOffset = 0x44;
constexpr std::size_t kSprintTriggerOffset = 0x4C;

struct UpdateDraft {
    HeroDiagnostics* owner{};
    void* weapon{};
    HeroSubject* subject{};
    trace::MeleeTickRecord record{};
    std::uint16_t flags{};
};

struct NetworkDraft {
    HeroDiagnostics* owner{};
    void* weapon{};
    HeroSubject* subject{};
    trace::NetworkStateRecord record{};
    std::uint16_t flags{};
};

struct TransitionDraft {
    HeroDiagnostics* owner{};
    void* weapon{};
    HeroSubject* subject{};
    trace::TransitionRecord record{};
    std::uint16_t flags{};
};

struct PresentationDraft {
    HeroDiagnostics* owner{};
    void* animator{};
    HeroSubject* subject{};
    trace::PresentationFrameRecord record{};
    std::uint16_t flags{};
};

struct InputDraft {
    HeroDiagnostics* owner{};
    void* weapon{};
    HeroSubject* subject{};
    trace::InputDecisionRecord record{};
    std::uint16_t flags{};
};

struct PredictionDraft {
    HeroDiagnostics* owner{};
    void* weapon{};
    HeroSubject* subject{};
    std::uint32_t instruction{};
    trace::TransitionRecord record{};
};

template <typename Value, std::size_t Capacity> struct DraftStack {
    [[nodiscard]] Value* push() noexcept {
        return size == values.size() ? nullptr : &values[size++];
    }

    [[nodiscard]] Value* current() noexcept {
        return size == 0 ? nullptr : &values[size - 1];
    }

    void pop() noexcept {
        if (size != 0) {
            values[--size] = {};
        }
    }

    std::array<Value, Capacity> values{};
    std::size_t size{};
    std::size_t overflow{};
};

thread_local DraftStack<UpdateDraft, 8> gUpdates;
thread_local DraftStack<NetworkDraft, 8> gNetworkStates;
thread_local DraftStack<TransitionDraft, 8> gTransitions;
thread_local DraftStack<PresentationDraft, 8> gPresentations;
thread_local DraftStack<InputDraft, 8> gInputs;
thread_local DraftStack<PredictionDraft, 8> gPredictions;
thread_local std::uint32_t gReadScope{};
std::atomic<std::uint32_t> gScope{};
PatchInstanceSlot<HeroDiagnostics> gActive;

template <typename Value>
[[nodiscard]] std::string_view number_text(Value value, std::array<char, 32>& buffer) noexcept {
    const auto [end, error] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    return error == std::errc{} ? std::string_view{buffer.data(), end} : std::string_view{"Unavailable"};
}

[[nodiscard]] std::string_view capture_name(CaptureMode mode) noexcept {
    switch (mode) {
    case CaptureMode::Standard:
        return "Standard";
    case CaptureMode::Combat:
        return "Combat";
    case CaptureMode::Full:
        return "Full";
    }
    std::unreachable();
}

[[nodiscard]] std::uint32_t pointer_id(const void* pointer) noexcept {
    return static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(pointer));
}

template <std::size_t Size>
void copy_floats(std::array<float, Size>& destination, const void* object, std::size_t offset) noexcept {
    if (object != nullptr) {
        std::memcpy(destination.data(), static_cast<const std::byte*>(object) + offset, sizeof(destination));
    }
}

[[nodiscard]] std::uint16_t subject_flags(const HeroSubject& subject) noexcept {
    return static_cast<std::uint16_t>(subject.local_player == 0xFF ? trace::RecordFlags::Remote
                                                                   : trace::RecordFlags::Local);
}

[[nodiscard]] std::uint16_t turn_flags(const trace::TurnContext& turn) noexcept {
    if (turn.local_turn != 0) {
        return trace::RecordFlags::Prediction;
    }
    return turn.update_pass != 0 ? trace::RecordFlags::Authority : trace::RecordFlags::None;
}

template <typename Draft> [[nodiscard]] Draft* begin_draft(DraftStack<Draft, 8>& stack) noexcept {
    auto* draft = stack.push();
    if (draft == nullptr) {
        ++stack.overflow;
    }
    return draft;
}

template <typename Draft> [[nodiscard]] Draft* finish_draft(DraftStack<Draft, 8>& stack) noexcept {
    if (stack.overflow != 0) {
        --stack.overflow;
        return nullptr;
    }
    return stack.current();
}

hero_melee_pipeline::DiagnosticsCallbacks make_pipeline_callbacks(HeroDiagnostics& diagnostics) noexcept {
    return {
        .context = &diagnostics,
        .before_update =
            [](void* context, void* weapon, float delta, const hero_melee_pipeline::UpdateDecision& decision) noexcept {
                static_cast<HeroDiagnostics*>(context)->begin_update(weapon, delta, decision);
            },
        .after_update =
            [](void* context, void* weapon, float delta, bool native_called, bool result) noexcept {
                static_cast<HeroDiagnostics*>(context)->finish_update(weapon, delta, native_called, result);
            },
        .before_network_state =
            [](void* context, void* weapon, int state, bool flag,
               const hero_melee_pipeline::NetworkStateDecision& decision) noexcept {
                static_cast<HeroDiagnostics*>(context)->begin_network_state(weapon, state, flag, decision);
            },
        .after_network_state =
            [](void* context, void* weapon, int state, bool flag, bool native_called) noexcept {
                static_cast<HeroDiagnostics*>(context)->finish_network_state(weapon, state, flag, native_called);
            },
        .before_enter_state =
            [](void* context, void* weapon, int state) noexcept {
                static_cast<HeroDiagnostics*>(context)->begin_enter_state(weapon, state);
            },
        .after_enter_state =
            [](void* context, void* weapon, int state) noexcept {
                static_cast<HeroDiagnostics*>(context)->finish_enter_state(weapon, state);
            },
        .before_animator_state =
            [](void* context, void* animator, std::uint32_t state, std::uint32_t active, std::uint32_t primary,
               std::uint32_t secondary) noexcept {
                static_cast<HeroDiagnostics*>(context)->begin_animator_state(animator, state, active, primary,
                                                                             secondary);
            },
        .after_animator_state =
            [](void* context, void* animator, std::uint32_t state, std::uint32_t active, std::uint32_t primary,
               std::uint32_t secondary, bool suppressed) noexcept {
                static_cast<HeroDiagnostics*>(context)->finish_animator_state(animator, state, active, primary,
                                                                              secondary, suppressed);
            },
        .input_queue =
            [](void* context, const MidHookContext& hook, bool after) noexcept {
                static_cast<HeroDiagnostics*>(context)->observe_input_queue(hook, after);
            },
        .prediction_transition =
            [](void* context, const MidHookContext& hook, bool after) noexcept {
                static_cast<HeroDiagnostics*>(context)->observe_prediction_transition(hook, after);
            },
    };
}

} // namespace

HeroDiagnostics::HeroDiagnostics(HeroDiagnosticsSettings settings, const TargetContext& target) noexcept
    : settings_(settings), target_(target),
      client_layout_(target.role == HostRole::Client ? &client_layout_for(target.layout) : nullptr),
      server_layout_(target.role == HostRole::Server ? &server_layout() : nullptr),
      subjects_(target, client_layout_ == nullptr
                            ? nullptr
                            : target.image.function_at_rva<GetJoystickIndex>(client_layout_->get_joystick_index_rva)),
      pipeline_callbacks_(make_pipeline_callbacks(*this))
#if !defined(FC_PATCH_BUILD_ROLE_MASK) || (FC_PATCH_BUILD_ROLE_MASK & 1) != 0
      ,
      soldier_state_observer_(make_soldier_state_observer(*this))
#endif
{
    if (client_layout_ != nullptr) {
        host_turn_ = target.image.read_at_rva<volatile std::int32_t>(client_layout_->host_turn_rva);
        client_turn_ = target.image.read_at_rva<volatile std::int32_t>(client_layout_->client_turn_rva);
        update_turn_ = target.image.read_at_rva<volatile std::int32_t>(client_layout_->update_turn_rva);
        predict_turn_ = target.image.read_at_rva<volatile std::int32_t>(client_layout_->predict_turn_rva);
        acknowledged_turn_ = target.image.read_at_rva<volatile std::int32_t>(client_layout_->acknowledged_turn_rva);
        destination_ = target.image.read_at_rva<volatile std::int32_t>(client_layout_->destination_rva);
        is_local_turn_ = target.image.read_at_rva<volatile std::uint8_t>(client_layout_->is_local_turn_rva);
        is_update_turn_ = target.image.read_at_rva<volatile std::uint8_t>(client_layout_->is_update_turn_rva);
        rollback_ = target.image.read_at_rva<volatile std::uint8_t>(client_layout_->rollback_rva);
        network_enabled_ = target.image.read_at_rva<volatile std::uint8_t>(client_layout_->network_enabled_rva);
        network_client_active_ =
            target.image.read_at_rva<volatile std::uint8_t>(client_layout_->network_client_active_rva);
        remote_moves_ = target.image.read_at_rva<std::byte>(client_layout_->remote_moves_rva);
    } else {
        host_turn_ = target.image.read_at_rva<volatile std::int32_t>(server_layout_->host_turn_rva);
        destination_ = target.image.read_at_rva<volatile std::int32_t>(server_layout_->destination_rva);
    }
}

HeroDiagnostics::~HeroDiagnostics() {
    disable_runtime();
}

void HeroDiagnostics::build_plan(PatchPlan& plan) {
    if (target_.role == HostRole::Client) {
#if !defined(FC_PATCH_BUILD_ROLE_MASK) || (FC_PATCH_BUILD_ROLE_MASK & 1) != 0
        build_client_plan(plan, target_, settings_.capture);
#endif
    } else {
#if !defined(FC_PATCH_BUILD_ROLE_MASK) || (FC_PATCH_BUILD_ROLE_MASK & 2) != 0
        build_server_plan(plan, target_, settings_.capture);
#endif
    }
}

std::expected<void, OutcomeReason> HeroDiagnostics::prepare_runtime() {
    return channel_.prepare(target_, "HeroDiagnostics", trace::kSchemaVersion,
                            static_cast<std::uint8_t>(std::to_underlying(settings_.capture)));
}

void HeroDiagnostics::enable_runtime() noexcept {
    subjects_.reset();
    channel_.start();
    publish_observers(*this);
#if !defined(FC_PATCH_BUILD_ROLE_MASK) || (FC_PATCH_BUILD_ROLE_MASK & 1) != 0
    if (target_.role == HostRole::Client) {
        hero_melee_pipeline::publish_diagnostics(pipeline_callbacks_);
        soldier_state_pipeline::publish_observer(soldier_state_observer_);
    }
#endif
}

void HeroDiagnostics::disable_runtime() noexcept {
#if !defined(FC_PATCH_BUILD_ROLE_MASK) || (FC_PATCH_BUILD_ROLE_MASK & 1) != 0
    if (target_.role == HostRole::Client) {
        soldier_state_pipeline::clear_observer(soldier_state_observer_);
        hero_melee_pipeline::clear_diagnostics(pipeline_callbacks_);
    }
#endif
    clear_observers(*this);
    channel_.stop();
}

void publish_observers(HeroDiagnostics& diagnostics) noexcept {
    gActive.publish(diagnostics);
}

void clear_observers(HeroDiagnostics& diagnostics) noexcept {
    gActive.clear(diagnostics);
}

HeroDiagnostics* active_diagnostics() noexcept {
    return gActive.read();
}

void HeroDiagnostics::write_status(StatusSection& output) const noexcept {
    const auto health = channel_.health();
    std::array<char, 32> buffer{};
    output.add("Capture", capture_name(settings_.capture));
    output.add("Trace", channel_.filename());
    output.add("EmittedRecords", number_text(health.emitted_records, buffer));
    output.add("Dropped", number_text(health.dropped, buffer));
    output.add("Omitted", number_text(health.omitted, buffer));
    output.add("EtwEventsLost", number_text(health.etw_events_lost, buffer));
    output.add("EtwBuffersLost", number_text(health.etw_buffers_lost, buffer));
    output.add("RingHighWater", number_text(health.high_water, buffer));
    output.add("UnexpectedThreadRecords", number_text(health.unexpected_thread_records, buffer));
    output.add("WriterErrors", number_text(health.writer_errors, buffer));
    output.add("State", health.file_limit_reached   ? "File limit reached"
                        : health.writer_errors != 0 ? "Writer error"
                                                    : "Recording");
}

CaptureMode HeroDiagnostics::capture_mode() const noexcept {
    return settings_.capture;
}

HeroSubject* HeroDiagnostics::bind(void* weapon) noexcept {
    auto* subject = subjects_.bind(weapon);
    if (subject != nullptr) {
        announce(*subject);
    }
    return subject;
}

HeroSubject* HeroDiagnostics::find_weapon(const void* weapon) noexcept {
    return subjects_.find_weapon(weapon);
}

HeroSubject* HeroDiagnostics::find_player(int player) noexcept {
    return subjects_.find_player(player);
}

HeroSubject* HeroDiagnostics::find_animator(const void* animator) noexcept {
    return subjects_.find_animator(animator);
}

HeroSubject* HeroDiagnostics::find_soldier(const void* soldier) noexcept {
    return subjects_.find_soldier(soldier);
}

HeroSubject* HeroDiagnostics::active_transition_subject() noexcept {
    auto* transition = gTransitions.current();
    return transition != nullptr && transition->owner == this ? transition->subject : nullptr;
}

trace::TurnContext HeroDiagnostics::turn_context() const noexcept {
    const auto active =
        target_.role == HostRole::Server || (network_enabled_ != nullptr && network_client_active_ != nullptr &&
                                             *network_enabled_ != 0 && *network_client_active_ != 0);
    auto update_turn = update_turn_ != nullptr ? *update_turn_ : -1;
    if (target_.role == HostRole::Server && host_turn_ != nullptr) {
        update_turn = *host_turn_;
    }
    return {
        .host_turn = host_turn_ != nullptr ? *host_turn_ : -1,
        .client_turn = client_turn_ != nullptr ? *client_turn_ : -1,
        .update_turn = update_turn,
        .predict_turn = predict_turn_ != nullptr ? *predict_turn_ : -1,
        .acknowledged_turn = acknowledged_turn_ != nullptr ? *acknowledged_turn_ : -1,
        .destination = destination_ != nullptr ? *destination_ : -1,
        .local_turn = is_local_turn_ != nullptr ? *is_local_turn_ : std::uint8_t{},
        .update_pass = is_update_turn_ != nullptr ? *is_update_turn_ : std::uint8_t{},
        .rollback = rollback_ != nullptr ? *rollback_ : std::uint8_t{},
        .network_active = static_cast<std::uint8_t>(active),
    };
}

std::uint16_t HeroDiagnostics::transition_flags(const void* weapon, const trace::TurnContext& turn) const noexcept {
    const auto* network_state = gNetworkStates.current();
    // SetNetworkState can enter or exit a state while replay-turn globals still describe local prediction.
    if (network_state != nullptr && network_state->owner == this && network_state->weapon == weapon) {
        return trace::RecordFlags::Authority;
    }
    return turn_flags(turn);
}

std::uint32_t HeroDiagnostics::state_fingerprint(const HeroSubject& subject) const noexcept {
    const auto state = read_native_field<int>(subject.weapon, kStateOffset);
    const auto state_count = read_native_field<std::uint8_t>(subject.combo, 0x21);
    const auto* rows = read_native_field<const std::byte*>(subject.combo, 0x10);
    if (state < 0 || state >= state_count || rows == nullptr) {
        return 0;
    }

    std::uint32_t hash = 2166136261U;
    const auto* bytes = rows + static_cast<std::size_t>(state) * 0x34;
    for (std::size_t index{}; index < 0x34; ++index) {
        hash ^= std::to_integer<std::uint8_t>(bytes[index]);
        hash *= 16777619U;
    }
    return hash;
}

void HeroDiagnostics::announce(HeroSubject& subject) noexcept {
    if (subject.announced) {
        return;
    }
    subject.announced = true;
    const trace::SubjectRecord record{
        .subject = subject.id,
        .generation = subject.generation,
        .weapon = pointer_id(subject.weapon),
        .owner = pointer_id(subject.owner),
        .soldier = pointer_id(subject.soldier),
        .animator = pointer_id(subject.animator),
        .combo = pointer_id(subject.combo),
        .player = subject.player,
        .local_player = subject.local_player,
        .role = static_cast<std::uint8_t>(target_.role),
    };
    channel_.submit(static_cast<std::uint16_t>(trace::RecordKind::Subject), trace::payload_bytes(record), subject.id,
                    subject_flags(subject));
}

void HeroDiagnostics::submit(trace::RecordKind kind, std::span<const std::byte> payload, const HeroSubject& subject,
                             std::uint16_t flags) noexcept {
    channel_.submit(static_cast<std::uint16_t>(kind), payload, subject.id,
                    static_cast<std::uint16_t>(flags | subject_flags(subject)));
}

std::uint32_t HeroDiagnostics::begin_read_scope() noexcept {
    const auto previous = gReadScope;
    gReadScope = next_scope();
    return previous;
}

void HeroDiagnostics::end_read_scope(std::uint32_t previous) noexcept {
    gReadScope = previous;
}

std::uint32_t HeroDiagnostics::current_read_scope() const noexcept {
    return gReadScope;
}

std::uint32_t HeroDiagnostics::next_scope() noexcept {
    return gScope.fetch_add(1, std::memory_order_relaxed) + 1;
}

// Captures one complete native melee update without changing its ownership decision.
void HeroDiagnostics::begin_update(void* weapon, float delta,
                                   const hero_melee_pipeline::UpdateDecision& decision) noexcept {
    auto* draft = begin_draft(gUpdates);
    if (draft == nullptr) {
        channel_.omit();
        return;
    }
    *draft = {.owner = this, .weapon = weapon};
    auto* subject = bind(weapon);
    if (subject == nullptr) {
        return;
    }
    draft->subject = subject;
    auto& record = draft->record;
    record.turn = turn_context();
    record.delta = delta;
    record.state_time_before = read_native_field<float>(weapon, kStateTimeOffset);
    record.input_time_before = read_native_field<float>(weapon, kInputTimeOffset);
    record.state_fingerprint = state_fingerprint(*subject);
    record.state_before = static_cast<std::int8_t>(read_native_field<int>(weapon, kStateOffset));
    record.previous_before = static_cast<std::int8_t>(read_native_field<int>(weapon, kPreviousStateOffset));
    record.decision = decision.call_native ? 0 : 1;
    draft->flags = turn_flags(record.turn);

    if (settings_.capture != CaptureMode::Standard && subject->local_player == 0xFF && remote_moves_ != nullptr) {
        std::array<std::uint32_t, 6> move{};
        std::memcpy(move.data(), remote_moves_ + static_cast<std::size_t>(subject->player) * kRemoteMoveStride,
                    sizeof(move));
        const auto changed = !subject->consumed_move_seen || move != subject->last_consumed_move;
        subject->last_consumed_move = move;
        subject->consumed_move_seen = true;
        trace::NetworkMovementRecord movement{
            .turn = record.turn,
            .axes = {move[0], move[1], move[2], move[3]},
            .buttons = move[4],
            .triggers =
                {
                    read_native_field<std::uint32_t>(subject->owner, kPrimaryTriggerOffset),
                    read_native_field<std::uint32_t>(subject->owner, kSecondaryTriggerOffset),
                    read_native_field<std::uint32_t>(subject->owner, kSprintTriggerOffset),
                    read_native_field<std::uint32_t>(subject->owner, kJumpTriggerOffset),
                },
            .state = record.state_before,
            .kind = trace::NetworkMovementKind::ClientConsumed,
            .changed = static_cast<std::uint8_t>(changed),
        };
        copy_floats(movement.position, subject->soldier, kPositionOffset);
        copy_floats(movement.velocity, subject->soldier, kVelocityOffset);
        submit(trace::RecordKind::NetworkMovement, trace::payload_bytes(movement), *subject, draft->flags);
    }
}

void HeroDiagnostics::finish_update(void* weapon, float, bool native_called, bool result) noexcept {
    auto* draft = finish_draft(gUpdates);
    if (draft == nullptr) {
        return;
    }
    if (draft->owner == this && draft->weapon == weapon && draft->subject != nullptr) {
        auto& record = draft->record;
        record.state_time_after = read_native_field<float>(weapon, kStateTimeOffset);
        record.input_time_after = read_native_field<float>(weapon, kInputTimeOffset);
        record.state_after = static_cast<std::int8_t>(read_native_field<int>(weapon, kStateOffset));
        record.previous_after = static_cast<std::int8_t>(read_native_field<int>(weapon, kPreviousStateOffset));
        record.native_called = static_cast<std::uint8_t>(native_called);
        record.result = static_cast<std::uint8_t>(result);
        copy_floats(record.position, draft->subject->soldier, kPositionOffset);
        copy_floats(record.velocity, draft->subject->soldier, kVelocityOffset);
        auto flags = draft->flags;
        if (native_called) {
            flags |= trace::RecordFlags::NativeCalled;
        } else {
            flags |= trace::RecordFlags::Suppressed;
        }
        if (result) {
            flags |= trace::RecordFlags::NativeResult;
        }
        if (record.state_before != record.state_after) {
            flags |= trace::RecordFlags::StateChanged;
        }
        submit(trace::RecordKind::MeleeTick, trace::payload_bytes(record), *draft->subject, flags);
    }
    gUpdates.pop();
}

void HeroDiagnostics::begin_network_state(void* weapon, int, bool flag,
                                          const hero_melee_pipeline::NetworkStateDecision& decision) noexcept {
    auto* draft = begin_draft(gNetworkStates);
    if (draft == nullptr) {
        channel_.omit();
        return;
    }
    *draft = {.owner = this, .weapon = weapon};
    auto* subject = bind(weapon);
    if (subject == nullptr) {
        return;
    }
    draft->subject = subject;
    auto& record = draft->record;
    record.turn = turn_context();
    record.state_before = read_native_field<int>(weapon, kStateOffset);
    record.requested_state = decision.state;
    record.state_time_before = read_native_field<float>(weapon, kStateTimeOffset);
    record.state_fingerprint = state_fingerprint(*subject);
    record.scope = gReadScope;
    record.set_flag = static_cast<std::uint8_t>(flag);
    record.operation = trace::NetworkStateOperation::Apply;
    draft->flags = trace::RecordFlags::Authority;
}

void HeroDiagnostics::finish_network_state(void* weapon, int, bool, bool native_called) noexcept {
    auto* draft = finish_draft(gNetworkStates);
    if (draft == nullptr) {
        return;
    }
    if (draft->owner == this && draft->weapon == weapon && draft->subject != nullptr) {
        auto& record = draft->record;
        record.state_after = read_native_field<int>(weapon, kStateOffset);
        record.state_time_after = read_native_field<float>(weapon, kStateTimeOffset);
        record.native_called = static_cast<std::uint8_t>(native_called);
        auto flags = draft->flags;
        if (native_called) {
            flags |= trace::RecordFlags::NativeCalled;
        } else {
            flags |= trace::RecordFlags::Suppressed;
        }
        if (record.state_before != record.state_after) {
            flags |= trace::RecordFlags::StateChanged;
        }
        submit(trace::RecordKind::NetworkState, trace::payload_bytes(record), *draft->subject, flags);
    }
    gNetworkStates.pop();
}

void HeroDiagnostics::begin_enter_state(void* weapon, int state) noexcept {
    auto* draft = begin_draft(gTransitions);
    if (draft == nullptr) {
        channel_.omit();
        return;
    }
    *draft = {.owner = this, .weapon = weapon};
    auto* subject = bind(weapon);
    if (subject == nullptr) {
        return;
    }
    draft->subject = subject;
    auto& record = draft->record;
    record.turn = turn_context();
    record.requested_state = state;
    record.state_before = read_native_field<int>(weapon, kStateOffset);
    record.state_time_before = read_native_field<float>(weapon, kStateTimeOffset);
    record.kind = trace::TransitionKind::Enter;
    draft->flags = transition_flags(weapon, record.turn);
}

void HeroDiagnostics::finish_enter_state(void* weapon, int) noexcept {
    auto* draft = finish_draft(gTransitions);
    if (draft == nullptr) {
        return;
    }
    if (draft->owner == this && draft->weapon == weapon && draft->subject != nullptr) {
        auto& record = draft->record;
        record.state_after = read_native_field<int>(weapon, kStateOffset);
        record.previous_state = read_native_field<int>(weapon, kPreviousStateOffset);
        record.state_time_after = read_native_field<float>(weapon, kStateTimeOffset);
        record.state_fingerprint = state_fingerprint(*draft->subject);
        copy_floats(record.position, draft->subject->soldier, kPositionOffset);
        copy_floats(record.velocity, draft->subject->soldier, kVelocityOffset);
        auto flags = static_cast<std::uint16_t>(draft->flags | trace::RecordFlags::NativeCalled);
        if (record.state_before != record.state_after) {
            flags |= trace::RecordFlags::StateChanged;
        }
        submit(trace::RecordKind::Transition, trace::payload_bytes(record), *draft->subject, flags);
    }
    gTransitions.pop();
}

void HeroDiagnostics::begin_animator_state(void* animator, std::uint32_t state, std::uint32_t active,
                                           std::uint32_t primary, std::uint32_t secondary) noexcept {
    auto* draft = begin_draft(gPresentations);
    if (draft == nullptr) {
        channel_.omit();
        return;
    }
    *draft = {.owner = this, .animator = animator};
    if (settings_.capture != CaptureMode::Full) {
        return;
    }
    auto* subject = find_animator(animator);
    if (subject == nullptr) {
        return;
    }
    draft->subject = subject;
    draft->record = {
        .turn = turn_context(),
        .argument0 = state,
        .argument1 = active,
        .state = static_cast<std::int8_t>(read_native_field<int>(subject->weapon, kStateOffset)),
        .kind = trace::PresentationKind::Combo,
    };
    draft->record.upper_clip = primary;
    draft->record.lower_clip = secondary;
}

void HeroDiagnostics::finish_animator_state(void* animator, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t,
                                            bool suppressed) noexcept {
    auto* draft = finish_draft(gPresentations);
    if (draft == nullptr) {
        return;
    }
    if (draft->owner == this && draft->animator == animator && draft->subject != nullptr) {
        if (suppressed) {
            draft->record.status |= trace::PresentationSuppressed;
        }
        auto flags = suppressed ? trace::RecordFlags::Suppressed : trace::RecordFlags::NativeCalled;
        submit(trace::RecordKind::PresentationFrame, trace::payload_bytes(draft->record), *draft->subject, flags);
    }
    gPresentations.pop();
}

void HeroDiagnostics::observe_input_queue(const MidHookContext& context, bool after) noexcept {
    if (settings_.capture == CaptureMode::Standard || context.ecx < kInputQueueOffset) {
        return;
    }
    if (!after) {
        auto* draft = begin_draft(gInputs);
        if (draft == nullptr) {
            channel_.omit();
            return;
        }
        auto* weapon = reinterpret_cast<void*>(context.ecx - kInputQueueOffset);
        *draft = {.owner = this, .weapon = weapon};
        auto* subject = bind(weapon);
        if (subject == nullptr) {
            return;
        }
        draft->subject = subject;
        auto* queue = reinterpret_cast<void*>(context.ecx);
        draft->record = {
            .turn = turn_context(),
            .state = static_cast<std::uint32_t>(read_native_field<int>(weapon, kStateOffset)),
            .buttons = read_native_field<std::uint8_t>(reinterpret_cast<const void*>(context.esp + sizeof(void*))),
            .down_before = read_native_field<std::uint8_t>(queue, kInputQueueDownOffset),
            .queue_count = read_native_field<std::uint8_t>(queue, kInputQueueCountOffset),
            .queue_head = read_native_field<std::uint8_t>(queue, kInputQueueHeadOffset),
        };
        draft->flags = turn_flags(draft->record.turn);
        return;
    }

    auto* draft = finish_draft(gInputs);
    if (draft == nullptr) {
        return;
    }
    if (draft->owner == this && draft->weapon == reinterpret_cast<void*>(context.ecx - kInputQueueOffset) &&
        draft->subject != nullptr) {
        draft->record.down_after =
            read_native_field<std::uint8_t>(reinterpret_cast<void*>(context.ecx), kInputQueueDownOffset);
        submit(trace::RecordKind::InputDecision, trace::payload_bytes(draft->record), *draft->subject, draft->flags);
    }
    gInputs.pop();
}

void HeroDiagnostics::observe_prediction_transition(const MidHookContext& context, bool after) noexcept {
    if (settings_.capture == CaptureMode::Standard) {
        return;
    }

    if (!after) {
        auto* draft = begin_draft(gPredictions);
        if (draft == nullptr) {
            channel_.omit();
            return;
        }
        auto* weapon = reinterpret_cast<void*>(context.ebx);
        *draft = {.owner = this, .weapon = weapon, .instruction = context.eip};
        auto* subject = find_weapon(weapon);
        if (subject == nullptr) {
            return;
        }
        draft->subject = subject;
        draft->record = {
            .turn = turn_context(),
            .requested_state = static_cast<std::int32_t>(context.esi),
            .state_before = read_native_field<int>(weapon, kStateOffset),
            .state_after = read_native_field<int>(weapon, kStateOffset),
            .previous_state = read_native_field<int>(weapon, kPreviousStateOffset),
            .state_time_before = read_native_field<float>(weapon, kStateTimeOffset),
            .state_time_after = read_native_field<float>(weapon, kStateTimeOffset),
            .state_fingerprint = state_fingerprint(*subject),
            .kind = trace::TransitionKind::Prediction,
        };
        return;
    }

    auto* draft = finish_draft(gPredictions);
    if (draft == nullptr) {
        return;
    }
    if (draft->owner == this && draft->weapon == reinterpret_cast<void*>(context.ebx) && draft->subject != nullptr) {
        auto flags = static_cast<std::uint16_t>(trace::RecordFlags::Prediction);
        if (draft->instruction != context.eip) {
            flags |= trace::RecordFlags::Suppressed;
        }
        submit(trace::RecordKind::Transition, trace::payload_bytes(draft->record), *draft->subject, flags);
    }
    gPredictions.pop();
}

} // namespace fusioncutter::patches::hero_diagnostics
