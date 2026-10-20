#include "multiplayer.hpp"

#include "core_runtime.hpp"
#include "fatal_boundary.hpp"
#include "message_protocol.hpp"
#include "patches/patch_api.hpp"
#include "patches/patch_record.hpp"
#include "reporting/status.hpp"

#include <fc/reporting.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <limits>
#include <iterator>
#include <format>
#include <utility>

namespace fc::runtime {

namespace {
// Creates a diagnostic naming the patch whose prepare() operation failed.
Diagnostic error(const FC_Feature &feature, std::string message) {
    return {qualified_patch_id(*feature.owner), std::move(message)};
}

// Names the resulting feature agreement without exposing negotiation steps.
std::string_view agreement_name(Agreement agreement) noexcept {
    switch (agreement) {
    case Agreement::Unavailable:
        return "Unavailable";
    case Agreement::Pending:
        return "Pending";
    case Agreement::Compatible:
        return "Compatible";
    case Agreement::Incompatible:
        return "Incompatible";
    case Agreement::Failed:
        return "Failed";
    }
    return "Unavailable";
}
} // namespace

PeerId MultiplayerService::next_peer() noexcept {
    static std::atomic<std::uint64_t> next{};
    const auto value = next.fetch_add(1, std::memory_order_relaxed);
    assert(value < std::numeric_limits<std::uint64_t>::max());
    return {value + 1};
}

void MultiplayerService::log(FC_LogLevel level, std::string_view text) const {
    for (const auto &feature : features_) {
        if (feature->frozen && feature->owner->runtime) {
            feature->owner->runtime->reporting().write(nullptr, level, fc::detail::text(text));
            return;
        }
    }
}

std::string_view MultiplayerService::pending_step(Phase phase) noexcept {
    switch (phase) {
    case Phase::AwaitHello:
        return "waiting for Hello";
    case Phase::Hello:
        return "submitting Hello";
    case Phase::AwaitWelcome:
        return "waiting for Welcome";
    case Phase::Welcome:
        return "submitting Welcome";
    case Phase::AwaitReady:
        return "waiting for Ready";
    case Phase::Ready:
        return "submitting Ready";
    case Phase::Established:
        return "negotiation completed";
    case Phase::Expired:
        return "negotiation expired";
    case Phase::Failed:
        return "connection failed";
    }
    return {};
}

void MultiplayerService::timeout(Connection &connection, Clock::time_point now) {
    if (connection.local || connection.phase == Phase::Established || connection.phase == Phase::Failed ||
        connection.phase == Phase::Expired || now < connection.deadline) {
        return;
    }
    log(FC_LOG_DEBUG, std::format("Multiplayer peer {} at native slot {} destination {}: negotiation timed out after 3 seconds while {}",
                                  connection.id.value, connection.native_slot, connection.raw, pending_step(connection.phase)));
    connection.phase = Phase::Expired;
}

void MultiplayerService::reject(Connection &connection, Rejection cause, std::string_view reason) {
    const auto flag = static_cast<std::uint8_t>(cause);
    if (connection.phase == Phase::Established || connection.phase == Phase::Expired || connection.phase == Phase::Failed ||
        (connection.rejected & flag)) {
        return;
    }
    connection.rejected |= flag;
    log(FC_LOG_DEBUG, std::format("Multiplayer peer {} at native slot {} destination {}: rejected packet: {} while {}", connection.id.value,
                                  connection.native_slot, connection.raw, reason, pending_step(connection.phase)));
}

void MultiplayerService::discard_messages(Connection &connection) noexcept {
    for (const auto &message : connection.outgoing) {
        outgoing_bytes_ -= message.charge();
    }

    connection.outgoing.clear();

    std::erase_if(incoming_, [&](const IncomingMessage &message) {
        if (message.connection != connection.id) {
            return false;
        }
        incoming_bytes_ -= message.charge();
        return true;
    });
}

void MultiplayerService::expire(Connection &connection) {
    discard_messages(connection);
    connection = {};
}

void MultiplayerService::begin(GameState state, bool galaxy_selected) {
    std::lock_guard lock(mutex_);
    expire_hellos(Clock::time_point::max());
    for (auto &connection : connections_) {
        expire(connection);
    }
    host_ = local_ = {};
    host_info_ = local_info_ = {};
    mode_ = state.mode;
    unknown_sender_logged_ = false;
    selected_ = galaxy_selected && state.session &&
                (state.mode == GameMode::Host || state.mode == GameMode::Client || state.mode == GameMode::Dedicated);
    const std::uint32_t applicable = state.mode == GameMode::Host     ? FC_FEATURE_BOTH
                                     : state.mode == GameMode::Client ? FC_FEATURE_CLIENT
                                                                      : FC_FEATURE_HOST;

    client_manifest_.clear();
    host_manifest_.clear();
    feature_ids_.clear();
    for (auto &feature : features_) {
        feature->frozen =
            selected_ && feature->owner->state.load(std::memory_order_acquire) == PatchState::Installed ? feature->role & applicable : 0;
        if (feature->frozen & FC_FEATURE_CLIENT) {
            client_manifest_.push_back(feature->id);
        }
        if (feature->frozen & FC_FEATURE_HOST) {
            host_manifest_.push_back(feature->id);
        }
    }

    // Retain the exact frozen role lists and their unique union for every connection in this session.
    std::ranges::sort(client_manifest_);
    std::ranges::sort(host_manifest_);
    std::ranges::set_union(client_manifest_, host_manifest_, std::back_inserter(feature_ids_));

    // Validate each complete feature manifest before enabling remote messaging; an oversized manifest disables it.
    std::array<std::byte, message_protocol::kFrameBytes> frame{};
    remote_available_ = selected_ &&
                        (!(applicable & FC_FEATURE_CLIENT) ||
                         message_protocol::encode_manifest(message_protocol::Kind::Hello, {1, 0}, client_manifest_, frame)) &&
                        (!(applicable & FC_FEATURE_HOST) ||
                         message_protocol::encode_manifest(message_protocol::Kind::Welcome, {1, 1}, host_manifest_, frame));
    if (selected_ && !remote_available_) {
        for (const auto &feature : features_) {
            if (feature->frozen && feature->owner->runtime) {
                constexpr std::string_view text = "Multiplayer remote messaging is unavailable: the feature manifest exceeds 2048 bytes";
                feature->owner->runtime->reporting().write(nullptr, FC_LOG_ERROR, fc::detail::text(text));
                break;
            }
        }
    }
}

void MultiplayerService::end() {
    std::lock_guard lock(mutex_);
    expire_hellos(Clock::time_point::max());
    for (auto &connection : connections_) {
        expire(connection);
    }
    host_ = local_ = {};
    host_info_ = local_info_ = {};
    mode_ = {};
    client_manifest_.clear();
    host_manifest_.clear();
    feature_ids_.clear();
    selected_ = remote_available_ = false;
    for (auto &feature : features_) {
        feature->frozen = 0;
    }
}

void MultiplayerService::connect(std::uint32_t slot, std::uint64_t raw_address, std::optional<std::uint64_t> user_id, bool is_local) {
    std::lock_guard lock(mutex_);
    if (!selected_) {
        return;
    }
    const auto now = Clock::now();
    expire_hellos(now);

    auto found = std::ranges::find_if(connections_, [&](const Connection &connection) {
        return connection.id.value && connection.native_slot == slot;
    });
    if (found == connections_.end()) {
        found = std::ranges::find_if(connections_, [](const Connection &connection) {
            return !connection.id.value;
        });
        if (found == connections_.end()) {
            connections_.emplace_back();
            found = std::prev(connections_.end());
        }
    }

    auto &connection = *found;
    // Native roster copies can repeat Connect for a live slot; refresh its associations without starting another attempt.
    if (connection.id.value && connection.raw == raw_address) {
        connection.info.galaxy_id = user_id;
        if (connection.local) {
            local_info_.galaxy_id = user_id;
            host_info_.galaxy_id = user_id;
        }
        return;
    }

    if (connection.id == local_) {
        local_ = {};
    }
    if (mode_ == GameMode::Client && host_.value) {
        local_ = {};
    }

    expire(connection);
    connection.id = next_peer();
    connection.native_slot = slot;
    connection.raw = raw_address;
    connection.info = {{slot}, user_id};
    connection.local = is_local && mode_ == GameMode::Host;
    // Start the deadline at native registration, unless a retained Hello supplies an earlier deadline below.
    connection.deadline = now + std::chrono::seconds(3);
    for (const auto id : feature_ids_) {
        connection.paths.push_back({id});
    }

    if (mode_ == GameMode::Client) {
        for (auto &other : connections_) {
            if (&other != &connection) {
                expire(other);
            }
        }
        host_ = connection.id;
        if (!local_.value) {
            local_ = next_peer();
        }
        const auto random = token();
        connection.client_token = random.value_or(0);
        connection.phase = Phase::Hello;
        if (!random) {
            fail_connection(connection, "Could not generate a multiplayer connection token");
        }
    } else {
        if (!host_.value) {
            host_ = next_peer();
        }
        if (connection.local) {
            local_ = connection.id;
            local_info_ = connection.info;
            host_info_.galaxy_id = user_id;
            connection.phase = Phase::Established;
        }
    }
    if (!connection.local) {
        log(FC_LOG_DEBUG, std::format("Multiplayer peer {} at native slot {} destination {}: negotiation started; {}", connection.id.value,
                                      slot, raw_address, pending_step(connection.phase)));
    }

    const auto hello = std::ranges::find_if(pending_hellos_, [&](const PendingHello &pending) {
        return pending.sender == raw_address;
    });
    if (hello != pending_hellos_.end()) {
        if (!connection.local && mode_ != GameMode::Client) {
            connection.deadline = hello->deadline;
            const auto frame = message_protocol::parse(hello->bytes);
            // Derive support before releasing the retained frame's storage.
            accept_hello(connection, frame->tokens.client, frame->manifest);
        }
        incoming_bytes_ -= hello->charge();
        pending_hellos_.erase(hello);
    }
}

void MultiplayerService::disconnect(std::uint32_t slot) {
    std::lock_guard lock(mutex_);
    const auto found = std::ranges::find_if(connections_, [&](const Connection &connection) {
        return connection.id.value && connection.native_slot == slot;
    });
    if (found == connections_.end()) {
        return;
    }

    auto &connection = *found;
    if (connection.id == local_) {
        local_ = {};
        local_info_ = {};
    }
    if (connection.id == host_) {
        host_ = local_ = {};
        local_info_ = {};
    }
    expire(connection);
}

void MultiplayerService::reset_connections() {
    std::lock_guard lock(mutex_);
    expire_hellos(Clock::time_point::max());
    for (auto &connection : connections_) {
        expire(connection);
    }
    host_ = local_ = {};
    host_info_ = local_info_ = {};
}

void MultiplayerService::observe_local(PeerInfo info) {
    std::lock_guard lock(mutex_);
    if (!selected_) {
        return;
    }

    local_info_ = info;
    if (mode_ == GameMode::Host || mode_ == GameMode::Dedicated) {
        if (!host_.value) {
            host_ = next_peer();
        }
        host_info_.galaxy_id = info.galaxy_id;
    } else if (mode_ == GameMode::Client && !local_.value) {
        local_ = next_peer();
    }
    if (mode_ == GameMode::Host && local_.value) {
        for (auto &connection : connections_) {
            if (connection.id == local_) {
                connection.info = info;
            }
        }
    }
}

PeerId MultiplayerService::host() const {
    std::lock_guard lock(mutex_);
    return host_;
}

PeerId MultiplayerService::local() const {
    std::lock_guard lock(mutex_);
    return local_;
}

std::vector<PeerId> MultiplayerService::clients() const {
    std::lock_guard lock(mutex_);
    std::vector<PeerId> peers;
    if (mode_ == GameMode::Host || mode_ == GameMode::Dedicated) {
        for (const auto &connection : connections_) {
            if (connection.id.value) {
                peers.push_back(connection.id);
            }
        }
    }
    return peers;
}

std::optional<PeerInfo> MultiplayerService::info(PeerId peer) const {
    std::lock_guard lock(mutex_);
    if (!peer.value) {
        return std::nullopt;
    }

    for (const auto &connection : connections_) {
        if (connection.id == peer) {
            return connection.info;
        }
    }
    if (peer == host_) {
        return host_info_;
    }
    return peer == local_ ? std::optional{local_info_} : std::nullopt;
}

std::expected<FC_Feature *, Diagnostic> MultiplayerService::feature(PatchRecord &owner, std::string_view id, std::uint32_t role) {
    std::lock_guard lock(mutex_);
    if (owner.state.load(std::memory_order_acquire) != PatchState::Preparing) {
        return std::unexpected(Diagnostic{qualified_patch_id(owner), "Register multiplayer features during prepare()"});
    }
    if (!message_protocol::valid_feature_id(id) || role < FC_FEATURE_CLIENT || role > FC_FEATURE_BOTH) {
        return std::unexpected(Diagnostic{qualified_patch_id(owner), "Multiplayer feature ID or role is invalid"});
    }

    for (const auto &feature : features_) {
        if (feature->id == id && (feature->role & role)) {
            return std::unexpected(
                Diagnostic{qualified_patch_id(owner), "A requested role is already registered for this multiplayer feature ID"});
        }
    }

    auto record = std::make_unique<FC_Feature>();
    record->service = this;
    record->owner = &owner;
    record->id = id;
    record->role = role;
    record->payload_capacity = static_cast<std::uint32_t>(message_protocol::payload_capacity(record->id));
    auto *result = record.get();
    features_.push_back(std::move(record));
    return result;
}

std::expected<void, Diagnostic> MultiplayerService::on_peer(FC_Feature &feature, void *capture, FC_PeerChange callback) {
    std::lock_guard lock(mutex_);
    if (feature.owner->state.load(std::memory_order_acquire) != PatchState::Preparing || !callback) {
        return std::unexpected(error(feature, "Register a callable multiplayer peer callback during prepare()"));
    }

    feature.peers.push_back({capture, callback});
    return {};
}

std::expected<void, Diagnostic> MultiplayerService::on_message(FC_Feature &feature, void *capture, FC_FeatureMessage callback) {
    std::lock_guard lock(mutex_);
    if (feature.owner->state.load(std::memory_order_acquire) != PatchState::Preparing || !callback) {
        return std::unexpected(error(feature, "Register a callable multiplayer message callback during prepare()"));
    }

    feature.messages.push_back({capture, callback});
    return {};
}

FC_Feature *MultiplayerService::implementation(std::string_view id, std::uint32_t role) const noexcept {
    for (const auto &feature : features_) {
        if (feature->id == id && (feature->frozen & role)) {
            return feature.get();
        }
    }
    return nullptr;
}

MultiplayerService::Connection *MultiplayerService::route(const FC_Feature &feature, PeerId peer) noexcept {
    return const_cast<Connection *>(std::as_const(*this).route(feature, peer));
}

const MultiplayerService::Connection *MultiplayerService::route(const FC_Feature &feature, PeerId peer) const noexcept {
    if (!peer.value || !feature.frozen) {
        return nullptr;
    }

    if ((feature.frozen & FC_FEATURE_CLIENT) && peer == host_) {
        if (mode_ == GameMode::Client) {
            const auto found = std::ranges::find_if(connections_, [&](const Connection &connection) {
                return connection.id == host_;
            });
            return found == connections_.end() ? nullptr : &*found;
        }
        for (const auto &connection : connections_) {
            if (connection.id == local_ && local_.value) {
                return &connection;
            }
        }
    }

    if (feature.frozen & FC_FEATURE_HOST) {
        for (const auto &connection : connections_) {
            if (connection.id == peer) {
                return &connection;
            }
        }
    }
    return nullptr;
}

Agreement MultiplayerService::agreement_locked(const FC_Feature &feature, PeerId peer) const {
    if (feature.owner->state.load(std::memory_order_acquire) != PatchState::Installed) {
        return Agreement::Unavailable;
    }
    const auto *connection = route(feature, peer);
    if (!connection || (!connection->local && !remote_available_)) {
        return Agreement::Unavailable;
    }
    const auto path = std::ranges::find(connection->paths, feature.id, &FeaturePath::id);
    if (connection->phase == Phase::Failed || (path != connection->paths.end() && path->failed)) {
        return Agreement::Failed;
    }
    if (connection->phase == Phase::Expired) {
        return Agreement::Incompatible;
    }
    if (connection->phase != Phase::Established) {
        return Agreement::Pending;
    }
    const bool supported = connection->local ? implementation(feature.id, peer == host_ ? FC_FEATURE_HOST : FC_FEATURE_CLIENT) != nullptr
                                             : path != connection->paths.end() && path->remote_supported;
    return supported ? Agreement::Compatible : Agreement::Incompatible;
}

Agreement MultiplayerService::agreement(const FC_Feature &feature, PeerId peer) const {
    std::lock_guard lock(mutex_);
    return agreement_locked(feature, peer);
}

std::uint32_t MultiplayerService::max_payload(const FC_Feature &feature) const noexcept {
    return feature.payload_capacity;
}

SendResult MultiplayerService::send(FC_Feature &feature, PeerId peer, Bytes bytes) noexcept {
    std::unique_lock lock(mutex_, std::try_to_lock);
    if (!lock) {
        return SendResult::Full;
    }

    switch (agreement_locked(feature, peer)) {
    case Agreement::Unavailable:
        return SendResult::Unavailable;
    case Agreement::Pending:
        return SendResult::Pending;
    case Agreement::Incompatible:
        return SendResult::Incompatible;
    case Agreement::Failed:
        return SendResult::Failed;
    case Agreement::Compatible:
        break;
    }
    if (bytes.size() > max_payload(feature)) {
        return SendResult::TooLarge;
    }

    const auto charge = sizeof(OutgoingMessage) + bytes.size();
    if (charge > kQueueBytes - outgoing_bytes_) {
        return SendResult::Full;
    }

    try {
        auto *connection = route(feature, peer);
        connection->outgoing.push_back({&feature, peer, {bytes.begin(), bytes.end()}});
        outgoing_bytes_ += charge;
        return SendResult::Accepted;
    } catch (...) {
        return SendResult::Full;
    }
}

void MultiplayerService::fail(FC_Feature &feature, PeerId peer, std::string_view reason) {
    std::lock_guard lock(mutex_);
    auto *connection = route(feature, peer);
    if (!connection) {
        return;
    }

    const auto found = std::ranges::find_if(connection->paths, [&](const FeaturePath &path) {
        return path.id == feature.id;
    });
    if (found == connection->paths.end() || found->failed) {
        return;
    }

    // Set failure flags before copying the reason, so even allocation failure preserves the communication result.
    found->failed = true;
    found->notice_pending = !connection->local;
    found->reason = reason;
    if (feature.owner->runtime) {
        fc::Logger{{feature.owner, &feature.owner->runtime->host()}}.error("Multiplayer delivery failed for peer {} feature '{}': {}",
                                                                           peer.value, feature.id, reason);
    }
    std::erase_if(connection->outgoing, [&](const OutgoingMessage &message) {
        if (message.feature->id != feature.id) {
            return false;
        }
        outgoing_bytes_ -= message.charge();
        return true;
    });
}

void MultiplayerService::deliver() {
    struct Notice {
        PatchRecord *owner;
        void *capture;
        FC_PeerChange callback;
        PeerId peer;
        Agreement agreement;
    };

    std::vector<Notice> notices;
    std::size_t message_count{};

    // Collect agreement changes while locked, then call patches unlocked so their callbacks can send replies.
    {
        std::lock_guard lock(mutex_);
        for (const auto &feature : features_) {
            if (feature->owner->state.load(std::memory_order_acquire) != PatchState::Installed) {
                continue;
            }
            std::map<std::uint64_t, Agreement> current;
            if ((feature->role & FC_FEATURE_CLIENT) && host_.value) {
                current.emplace(host_.value, agreement_locked(*feature, host_));
            }
            if (feature->role & FC_FEATURE_HOST) {
                if (mode_ != GameMode::Client) {
                    for (const auto &connection : connections_) {
                        if (connection.id.value) {
                            current.emplace(connection.id.value, agreement_locked(*feature, connection.id));
                        }
                    }
                }
            }
            std::vector<std::pair<std::uint64_t, Agreement>> changes;
            for (const auto &[peer, state] : current) {
                const auto previous = feature->last.find(peer);
                if (previous == feature->last.end() || previous->second != state) {
                    changes.emplace_back(peer, state);
                }
            }
            for (const auto &[peer, state] : feature->last) {
                if (!current.contains(peer)) {
                    changes.emplace_back(peer, Agreement::Unavailable);
                }
            }

            if (feature->owner->runtime) {
                auto &reporting = feature->owner->runtime->reporting();
                if (reporting.enabled(FC_LOG_DEBUG)) {
                    for (const auto &[peer, state] : changes) {
                        const auto *connection = route(*feature, {peer});
                        const bool absent =
                            state == Agreement::Incompatible && connection && !connection->local && connection->phase == Phase::Established;
                        const auto text = std::format("Multiplayer peer {} feature '{}': {}{}", peer, feature->id, agreement_name(state),
                                                      absent ? " (remote feature is absent from the advertised list)" : "");
                        reporting.write(feature->owner, FC_LOG_DEBUG, fc::detail::text(text));
                    }
                }
            }

            // Deliver every change to one callback before moving to the next.
            for (const auto &callback : feature->peers) {
                for (const auto &[peer, state] : changes) {
                    notices.push_back({feature->owner, callback.capture, callback.callback, {peer}, state});
                }
            }

            // Publish this pass's history before callbacks can change connections outside the lock.
            feature->last = std::move(current);
        }
        message_count = incoming_.size();
    }

    // Attribute each callback and any nested logging to the receiving patch.
    for (const auto &notice : notices) {
        if (notice.owner->state.load(std::memory_order_acquire) == PatchState::Installed) {
            PatchCallbackScope scope(notice.owner);
            fc::invoke_patch_callback([&] {
                notice.callback(notice.capture, notice.peer.value, static_cast<std::uint32_t>(notice.agreement));
            });
        }
    }

    // Use the sampled count to bound delivery even if native intake adds messages during callbacks.
    for (std::size_t i = 0; i < message_count; ++i) {
        IncomingMessage message;
        {
            std::lock_guard lock(mutex_);
            if (incoming_.empty() || agreement_locked(*incoming_.front().feature, incoming_.front().peer) == Agreement::Pending) {
                break;
            }
            message = std::move(incoming_.front());
            incoming_.pop_front();
        }

        // Keep this message in the incoming byte budget until its callbacks finish, including concurrent native intake.
        for (const auto &callback : message.feature->messages) {
            bool current{};
            {
                std::lock_guard lock(mutex_);
                const auto *connection = route(*message.feature, message.peer);
                current = connection && connection->id == message.connection &&
                          agreement_locked(*message.feature, message.peer) == Agreement::Compatible;
            }
            if (!current) {
                break;
            }
            PatchCallbackScope scope(message.feature->owner);
            fc::invoke_patch_callback([&] {
                callback.callback(
                    callback.capture, message.peer.value,
                    {reinterpret_cast<const std::uint8_t *>(message.bytes.data()), static_cast<std::uint32_t>(message.bytes.size())});
            });
        }

        {
            std::lock_guard lock(mutex_);
            const auto charge = message.charge();
            message = {};
            incoming_bytes_ -= charge;
        }
    }
}

void MultiplayerService::discard(PatchRecord &owner) {
    std::lock_guard lock(mutex_);
    std::erase_if(features_, [&](const auto &feature) {
        return feature->owner == &owner;
    });
}

void MultiplayerService::issues(fc::reporting::StatusText &status) const {
    std::vector<std::string> lines;
    {
        std::lock_guard lock(mutex_);
        if (selected_ && !remote_available_) {
            if (!status.content().empty()) {
                lines.emplace_back();
            }
            lines.emplace_back("Multiplayer remote messaging: Unavailable");
            lines.emplace_back("  The feature manifest at Game Start exceeds 2048 bytes");
        }
        for (const auto &connection : connections_) {
            if (!connection.id.value) {
                continue;
            }
            if (connection.phase == Phase::Failed) {
                if (!lines.empty() || !status.content().empty()) {
                    lines.emplace_back();
                }
                lines.push_back(std::format("Multiplayer connection at slot {}: Failed", connection.native_slot));
                lines.push_back("  " + (connection.failure_reason.empty() ? "Reliable delivery failed" : connection.failure_reason));
                continue;
            }
            for (const auto &path : connection.paths) {
                if (path.failed) {
                    if (!lines.empty() || !status.content().empty()) {
                        lines.emplace_back();
                    }
                    lines.push_back(std::format("Multiplayer feature {} at slot {}: Failed", path.id, connection.native_slot));
                    lines.push_back("  " + (path.reason.empty() ? "Communication failed" : path.reason));
                }
            }
        }
    }

    for (const auto &line : lines) {
        if (!status.line(line)) {
            break;
        }
    }
}

} // namespace fc::runtime
