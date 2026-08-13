#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

namespace fusioncutter::patches::network_diagnostics::trace {

inline constexpr std::uint32_t kSchemaVersion = 3;

enum class RecordKind : std::uint16_t {
    ClientFrame = 1,
    ServerFrame = 2,
    MoveSubmission = 3,
    ClientReceivePass = 4,
    TransportReceivePass = 5,
    ServerInputPass = 6,
    UpdateRead = 7,
    Prediction = 8,
    ClockAdjustment = 9,
    AuthorityTurn = 10,
    SendPass = 11,
    DestinationUpdate = 12,
    NativeSendGroup = 13,
    NativeReceiveGroup = 14,
    NativePacket = 15,
    Connection = 16,
    MoveEncoding = 17,
    MoveDecoding = 18,
    RemoteMoveBatch = 19,
    EventDecoding = 20,
    ReliableEventBatch = 21,
    OrdnanceEvent = 22,
    WeaponFire = 23,
    ControllerState = 24,
    ProjectileBuild = 25,
    ProjectileSimulation = 26,
    Damage = 27,
    ClientSoldierPose = 28,
    ServerSoldierPose = 29,
    NetworkPolicy = 30,
    DirectAssociation = 31,
};

[[nodiscard]] constexpr const char* record_kind_name(RecordKind kind) noexcept {
    constexpr std::array names{
        "Unknown",           "ClientFrame",          "ServerFrame",          "MoveSubmission",
        "ClientReceivePass", "TransportReceivePass", "ServerInputPass",      "UpdateRead",
        "Prediction",        "ClockAdjustment",      "AuthorityTurn",        "SendPass",
        "DestinationUpdate", "NativeSendGroup",      "NativeReceiveGroup",   "NativePacket",
        "Connection",        "MoveEncoding",         "MoveDecoding",         "RemoteMoveBatch",
        "EventDecoding",     "ReliableEventBatch",   "OrdnanceEvent",        "WeaponFire",
        "ControllerState",   "ProjectileBuild",      "ProjectileSimulation", "Damage",
        "ClientSoldierPose", "ServerSoldierPose",    "NetworkPolicy",        "DirectAssociation",
    };
    const auto index = std::to_underlying(kind);
    return index < names.size() ? names[index] : names.front();
}

enum class Carrier : std::uint8_t {
    Native = 1,
    Direct = 2,
    NotSent = 3,
    Unknown = 4,
};

enum RecordFlags : std::uint16_t {
    None = 0,
    Begin = 1U << 0U,
    End = 1U << 1U,
    Accepted = 1U << 2U,
    Rejected = 1U << 3U,
    Anomaly = 1U << 4U,
    Combat = 1U << 5U,
    Complete = 1U << 6U,
    Empty = 1U << 7U,
};

enum class ReadinessFailure : std::uint32_t {
    None = 0,
    NoLeadingTurn = 1,
    MissingInput = 2,
    ForceFillFailed = 3,
    ReadinessGate = 4,
};

enum class DirectAssociationPhase : std::uint8_t {
    Started = 1,
    Terminal = 2,
    Ended = 3,
};

enum class DirectRoute : std::uint8_t {
    Unclassified = 1,
    Negotiating = 2,
    Native = 3,
    Direct = 4,
};

#pragma pack(push, 1)

struct NativeSendGroupRecord {
    std::int32_t destination;
    std::uint32_t packets;
    std::uint32_t bytes;
    std::uint32_t native_packets;
    std::uint32_t direct_packets;
    std::uint32_t failed_packets;
    std::uint64_t duration;
};

struct TransportReceiveRecord {
    std::uint32_t native_packets;
    std::uint32_t direct_packets;
    std::uint32_t direct_data_packets;
    std::int32_t direct_slot{-1};
    std::uint32_t direct_generation;
    std::uint32_t direct_connection_id;
    std::uint32_t first_direct_sequence;
    std::uint32_t last_direct_sequence;
    std::uint64_t direct_player_mask;
    std::uint64_t duration;
};

struct NativePacketRecord {
    std::uint32_t endpoint;
    std::int32_t destination;
    std::uint32_t bytes;
    std::int32_t result;
};

struct ConnectionRecord {
    std::int32_t destination;
    std::uint32_t operation;
};

struct MoveSubmissionRecord {
    std::int32_t requested;
    std::int32_t turn_before;
    std::int32_t turn_after;
    float accumulator;
    std::uint64_t duration;
};

struct ClientReceivePassRecord {
    std::int32_t turn_before;
    std::int32_t turn_after;
    std::uint32_t candidates;
    std::uint32_t accepted;
    std::uint32_t stale;
    std::uint32_t newest_candidate;
    std::uint32_t recovered;
    std::uint32_t oldest_recovered;
    std::uint32_t newest_recovered;
    std::uint32_t reserved;
    std::uint64_t duration;
};

struct UpdateReadRecord {
    std::int32_t turn_before;
    std::int32_t turn_after;
    std::uint32_t group;
};

struct PredictionRecord {
    std::int32_t update_turn;
    std::int32_t predict_before;
    std::int32_t predict_after;
    std::int32_t local_turn;
    std::uint32_t result;
    std::uint32_t reserved;
    std::uint64_t duration;
};

struct ClockAdjustmentRecord {
    std::int32_t adjust_before;
    std::int32_t adjust_after;
    std::int32_t stable_before;
    std::int32_t stable_after;
    std::int32_t lag_before;
    std::int32_t lag_after;
    float scale_before;
    float scale_after;
};

struct ClientFrameRecord {
    float xmm0_delta;
    float xmm1_delta;
    std::uint64_t gap;
};

struct MoveCodecRecord {
    std::uint32_t packet;
    std::uint32_t previous;
    std::uint32_t move;
    std::uint32_t kind;
    std::int32_t player{-1};
    std::int32_t turn_slot{-1};
    std::int32_t turn_reference{-1};
    std::array<std::uint32_t, 4> axes;
    std::uint32_t buttons;
    std::uint32_t reserved;
    std::uint64_t duration;
};

struct FunctionRecord {
    std::uint32_t subject;
    std::int32_t value;
    std::uint32_t result;
    std::uint32_t reserved;
    std::uint64_t duration;
};

struct ReliableEventBatchRecord {
    std::uint32_t packet;
    std::uint32_t frontier_before;
    std::uint32_t frontier_after;
    std::uint32_t reserved;
    std::uint64_t duration;
};

struct NativeReceiveGroupRecord {
    std::uint32_t group;
    std::int32_t packet_type;
    std::uint32_t complete;
    std::uint32_t reserved;
    std::uint64_t duration;
};

struct ClientPoseRecord {
    std::uint32_t object;
    std::uint32_t related;
    std::uint32_t stage;
    std::int32_t player{-1};
    std::uint32_t mode;
    std::array<float, 16> matrix;
    std::uint32_t reserved;
    std::uint64_t duration;
};

struct AuthorityTurnRecord {
    std::int32_t candidate_turn;
    std::int32_t client_turn;
    std::int32_t host_turn_after;
    std::int32_t requested_turns;
    std::uint32_t catch_up_ordinal;
    std::uint32_t reserved;
    std::uint64_t required_mask;
    std::uint64_t received_mask;
    std::uint64_t missing_mask;
    std::uint64_t new_input_mask;
    std::uint64_t receive_duration;
    std::uint64_t rollback_duration;
    std::uint64_t save_duration;
    std::uint64_t simulation_duration;
    std::uint64_t finish_duration;
};

struct ServerFrameRecord {
    std::int32_t host_turn_before;
    std::int32_t host_turn_after;
    std::int32_t client_turn;
    std::uint16_t requested_turns;
    std::uint16_t turns_advanced;
    std::uint16_t receive_calls;
    std::uint16_t write_updates;
    ReadinessFailure terminal_failure;
    std::uint64_t destination_mask;
    std::uint64_t frame_gap;
    std::uint64_t send_duration;
    std::uint64_t write_update_duration;
    std::uint64_t required_mask;
    std::uint64_t missing_mask;
    float accumulator;
    float fixed_delta;
    float simulation_delta;
    float time_scale;
    std::uint32_t policy;
};

struct SchedulingRecord {
    std::uint32_t ack_slots;
    std::uint32_t rejected_slots;
    std::uint32_t create_fences;
    std::uint32_t blocked_destinations;
    std::uint32_t timed_out_fences;
    std::uint32_t maximum_pending_events;
    std::int32_t minimum_object_scale;
    std::uint32_t reserved;
    std::uint64_t destination_mask;
};

struct ServerInputRecord {
    std::uint32_t moves;
    std::uint32_t mini_moves;
    std::uint32_t filled_players;
    std::uint32_t filled_turns;
    std::uint32_t acknowledgements;
    std::int32_t oldest_acknowledged_turn{-1};
    std::int32_t newest_acknowledged_turn{-1};
    std::uint64_t acknowledged_players;
    std::uint64_t duration;
};

struct DestinationUpdateRecord {
    std::int32_t destination{-1};
    std::uint32_t stages;
    std::uint32_t event_cursor_before;
    std::uint32_t event_cursor_after;
    std::uint32_t event_head;
    std::uint32_t reserved;
    std::uint64_t objects_duration;
    std::uint64_t reliable_duration;
    std::uint64_t player_moves_duration;
    std::uint64_t events_duration;
};

struct WeaponFireRecord {
    std::uint32_t weapon;
    std::uint32_t owner;
    std::uint32_t aimer;
    std::uint32_t accepted;
    std::array<float, 16> matrix;
    std::uint64_t duration;
};

struct ProjectileBuildRecord {
    std::uint32_t projectile;
    std::uint32_t projectile_id;
    std::uint32_t projectile_class;
    std::uint32_t descriptor;
    std::uint32_t accepted;
    std::array<float, 3> position;
    std::array<float, 3> velocity;
    float lifetime;
    std::uint32_t reserved;
    std::uint64_t duration;
};

struct ProjectileSimulationRecord {
    std::uint32_t projectile;
    float delta;
    std::uint32_t rays;
    std::uint32_t ray_target;
    std::uint32_t ray_filter;
    std::uint32_t ray_filter_secondary;
    std::uint32_t ray_filter_word;
    std::uint32_t ray_flags;
    std::array<float, 3> position;
    std::array<float, 3> velocity;
    std::array<float, 3> origin;
    std::array<float, 3> direction;
    float ray_length;
    float ray_result;
    std::uint64_t duration;
};

struct DamageRecord {
    std::uint32_t damageable;
    std::uint32_t descriptor;
    std::uint32_t projectile;
    std::uint32_t projectile_id;
    std::uint32_t flags;
    float health_before;
    float health_after;
    float shield_before;
    float shield_after;
    std::uint32_t alive_after;
    std::uint32_t reserved;
    std::uint64_t duration;
};

struct ControllerStateRecord {
    std::uint32_t controller;
    std::uint32_t controllable;
    std::int32_t player;
    float delta;
    std::array<std::uint32_t, 4> axes;
    std::uint32_t buttons;
    std::uint32_t primary_switch_before;
    std::uint32_t primary_switch_after;
    std::uint32_t secondary_switch_before;
    std::uint32_t secondary_switch_after;
    std::uint32_t reserved;
    std::uint64_t duration;
};

struct OrdnanceEventRecord {
    std::uint32_t projectile_class;
    std::uint32_t descriptor;
    std::uint32_t head_before;
    std::uint32_t head_after;
    std::array<float, 3> origin;
    std::array<float, 3> direction;
};

struct SoldierPose {
    std::int32_t player;
    std::array<float, 3> position;
    std::array<float, 3> velocity;
};

struct ServerPoseBatch {
    std::int32_t turn;
    std::uint32_t count;
    std::array<SoldierPose, 3> soldiers;
};

struct NetworkPolicyRecord {
    std::uint8_t waitlate;
    std::uint8_t grace_turns;
    std::uint8_t network_enabled;
    std::uint8_t reserved;
    float fixed_delta;
};

struct DirectAssociationRecord {
    std::int32_t slot;
    std::uint32_t generation;
    std::uint32_t connection_id;
    DirectAssociationPhase phase;
    DirectRoute route;
    std::uint8_t route_reason;
    std::uint8_t end_reason;
    std::uint32_t attempts;
    std::uint32_t tx_datagrams;
    std::uint32_t rx_datagrams;
    std::uint32_t send_failures;
    std::uint32_t endpoint_rejects;
    std::uint32_t authentication_rejects;
    std::uint32_t replay_rejects;
    std::uint32_t invalid_rejects;
    std::uint64_t elapsed_ms;
    std::uint64_t direct_ms;
};

#pragma pack(pop)

template <typename Payload>
inline constexpr bool kValidPayload = std::is_trivially_copyable_v<Payload> && sizeof(Payload) <= 96;

static_assert(kValidPayload<NativeSendGroupRecord>);
static_assert(kValidPayload<TransportReceiveRecord>);
static_assert(kValidPayload<NativePacketRecord>);
static_assert(kValidPayload<ConnectionRecord>);
static_assert(kValidPayload<MoveSubmissionRecord>);
static_assert(kValidPayload<ClientReceivePassRecord>);
static_assert(kValidPayload<UpdateReadRecord>);
static_assert(kValidPayload<PredictionRecord>);
static_assert(kValidPayload<ClockAdjustmentRecord>);
static_assert(kValidPayload<ClientFrameRecord>);
static_assert(kValidPayload<MoveCodecRecord>);
static_assert(kValidPayload<FunctionRecord>);
static_assert(kValidPayload<ReliableEventBatchRecord>);
static_assert(kValidPayload<NativeReceiveGroupRecord>);
static_assert(sizeof(ClientPoseRecord) == 96);
static_assert(sizeof(AuthorityTurnRecord) == 96);
static_assert(sizeof(ServerFrameRecord) == 92);
static_assert(kValidPayload<SchedulingRecord>);
static_assert(kValidPayload<ServerInputRecord>);
static_assert(kValidPayload<DestinationUpdateRecord>);
static_assert(kValidPayload<WeaponFireRecord>);
static_assert(kValidPayload<ProjectileBuildRecord>);
static_assert(sizeof(ProjectileSimulationRecord) == 96);
static_assert(kValidPayload<DamageRecord>);
static_assert(kValidPayload<ControllerStateRecord>);
static_assert(kValidPayload<OrdnanceEventRecord>);
static_assert(kValidPayload<ServerPoseBatch>);
static_assert(kValidPayload<NetworkPolicyRecord>);
static_assert(kValidPayload<DirectAssociationRecord>);

} // namespace fusioncutter::patches::network_diagnostics::trace
