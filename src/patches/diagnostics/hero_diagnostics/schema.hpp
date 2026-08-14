#pragma once

#include <FusionCutter/diagnostics.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

namespace fusioncutter::patches::hero_diagnostics::trace {

inline constexpr std::uint32_t kSchemaVersion = 2;

enum class RecordKind : std::uint16_t {
    Subject = 1,
    MeleeTick = 2,
    InputDecision = 3,
    NetworkState = 4,
    Transition = 5,
    MovementFrame = 6,
    NetworkMovement = 7,
    PresentationFrame = 8,
    MeleeContact = 9,
    Deflection = 10,
    AudioCue = 11,
};

enum RecordFlags : std::uint16_t {
    None = 0,
    Begin = 1U << 0U,
    End = 1U << 1U,
    NativeCalled = 1U << 2U,
    NativeResult = 1U << 3U,
    StateChanged = 1U << 4U,
    Prediction = 1U << 5U,
    Authority = 1U << 6U,
    Local = 1U << 7U,
    Remote = 1U << 8U,
    Suppressed = 1U << 9U,
    Anomaly = 1U << 10U,
};

enum class MovementKind : std::uint8_t {
    Controls = 1,
    Velocity = 2,
    SoldierRead = 3,
};

enum class PresentationKind : std::uint8_t {
    Combo = 1,
    Action = 2,
    Pose = 3,
};

enum class TransitionKind : std::uint8_t {
    Enter = 1,
    Exit = 2,
    Prediction = 3,
};

enum class NetworkMovementKind : std::uint8_t {
    Parsed = 1,
    ClientConsumed = 2,
    ServerConsumed = 3,
};

enum class NetworkStateOperation : std::uint8_t {
    Apply = 1,
    Read = 2,
    Write = 3,
};

enum PresentationStatus : std::uint8_t {
    PresentationNormal = 0,
    PresentationSuppressed = 1U << 0U,
    UpperAnimationFinished = 1U << 1U,
    LowerAnimationFinished = 1U << 2U,
};

#pragma pack(push, 1)

struct TurnContext {
    std::int32_t host_turn{-1};
    std::int32_t client_turn{-1};
    std::int32_t update_turn{-1};
    std::int32_t predict_turn{-1};
    std::int32_t acknowledged_turn{-1};
    std::int32_t destination{-1};
    std::uint8_t local_turn{};
    std::uint8_t update_pass{};
    std::uint8_t rollback{};
    std::uint8_t network_active{};
};

struct SubjectRecord {
    std::uint32_t subject;
    std::uint32_t generation;
    std::uint32_t weapon;
    std::uint32_t owner;
    std::uint32_t soldier;
    std::uint32_t animator;
    std::uint32_t combo;
    std::int16_t player{-1};
    std::uint8_t local_player{0xFF};
    std::uint8_t role{};
};

struct MeleeTickRecord {
    TurnContext turn;
    float delta;
    float state_time_before;
    float state_time_after;
    float input_time_before;
    float input_time_after;
    std::array<float, 3> position;
    std::array<float, 3> velocity;
    std::uint32_t state_fingerprint;
    std::int8_t state_before{-1};
    std::int8_t state_after{-1};
    std::int8_t previous_before{-1};
    std::int8_t previous_after{-1};
    std::uint8_t decision{};
    std::uint8_t native_called{};
    std::uint8_t result{};
    std::uint8_t reserved{};
};

struct InputDecisionRecord {
    TurnContext turn;
    std::uint32_t state;
    std::uint8_t buttons{};
    std::uint8_t down_before{};
    std::uint8_t down_after{};
    std::uint8_t queue_count{};
    std::uint8_t queue_head{};
};

struct NetworkStateRecord {
    TurnContext turn;
    std::int32_t state_before{-1};
    std::int32_t requested_state{-1};
    std::int32_t state_after{-1};
    float state_time_before;
    float state_time_after;
    std::uint32_t state_fingerprint;
    std::uint32_t scope;
    std::uint32_t stream;
    std::uint16_t action{};
    std::uint8_t state_flags{};
    std::uint8_t set_flag{};
    std::uint8_t native_called{};
    NetworkStateOperation operation{};
};

struct TransitionRecord {
    TurnContext turn;
    std::int32_t requested_state{-1};
    std::int32_t state_before{-1};
    std::int32_t state_after{-1};
    std::int32_t previous_state{-1};
    float state_time_before;
    float state_time_after;
    std::uint32_t state_fingerprint;
    std::array<float, 3> position;
    std::array<float, 3> velocity;
    TransitionKind kind{};
    std::array<std::uint8_t, 3> reserved{};
};

struct MovementFrameRecord {
    TurnContext turn;
    std::array<float, 4> before;
    std::array<float, 4> after;
    std::array<float, 3> position;
    std::array<float, 3> velocity;
    std::int32_t state{-1};
    MovementKind kind{};
    std::uint8_t result{};
    std::array<std::uint8_t, 2> reserved{};
};

struct NetworkMovementRecord {
    TurnContext turn;
    std::array<std::uint32_t, 4> axes;
    std::uint32_t buttons;
    std::array<std::uint32_t, 4> triggers;
    std::array<float, 3> position;
    std::array<float, 3> velocity;
    std::int32_t state{-1};
    NetworkMovementKind kind{};
    std::uint8_t changed{};
    std::array<std::uint8_t, 2> reserved{};
};

struct PresentationFrameRecord {
    TurnContext turn;
    float state_time;
    float frame_delta;
    float extra_delta;
    float upper_time;
    float lower_time;
    float upper_timer;
    float lower_timer;
    float upper_blend;
    float lower_blend;
    float animation_rate;
    std::uint32_t upper_clip;
    std::uint32_t lower_clip;
    std::uint32_t action;
    std::uint32_t weapon_animation;
    std::uint32_t argument0;
    std::uint32_t argument1;
    std::int8_t state{-1};
    std::int8_t previous_state{-1};
    PresentationKind kind{};
    std::uint8_t status{};
};

struct MeleeContactRecord {
    TurnContext turn;
    std::uint32_t target;
    std::uint32_t filter;
    std::array<float, 3> position;
    std::int32_t state{-1};
    std::uint32_t state_fingerprint;
};

struct DeflectionRecord {
    TurnContext turn;
    std::uint32_t target;
    std::array<float, 3> input_position;
    std::array<float, 3> output_position;
    std::int32_t state{-1};
    std::uint8_t result{};
    std::array<std::uint8_t, 3> reserved{};
};

struct AudioCueRecord {
    TurnContext turn;
    std::uint32_t sound;
    std::uint32_t argument0;
    std::uint32_t argument1;
    std::uint32_t argument2;
    std::uint32_t argument3;
    std::int32_t state{-1};
};

#pragma pack(pop)

template <typename Payload>
    requires(std::is_trivially_copyable_v<Payload> && sizeof(Payload) <= diagnostics::kMaximumEtlPayloadSize)
[[nodiscard]] std::span<const std::byte> payload_bytes(const Payload& payload) noexcept {
    return std::as_bytes(std::span{&payload, std::size_t{1}});
}

template <typename Payload>
inline constexpr bool kValidPayload =
    std::is_trivially_copyable_v<Payload> && sizeof(Payload) <= diagnostics::kMaximumEtlPayloadSize;

static_assert(kValidPayload<SubjectRecord>);
static_assert(kValidPayload<MeleeTickRecord>);
static_assert(kValidPayload<InputDecisionRecord>);
static_assert(kValidPayload<NetworkStateRecord>);
static_assert(kValidPayload<TransitionRecord>);
static_assert(kValidPayload<MovementFrameRecord>);
static_assert(kValidPayload<NetworkMovementRecord>);
static_assert(kValidPayload<PresentationFrameRecord>);
static_assert(kValidPayload<MeleeContactRecord>);
static_assert(kValidPayload<DeflectionRecord>);
static_assert(kValidPayload<AudioCueRecord>);

} // namespace fusioncutter::patches::hero_diagnostics::trace
