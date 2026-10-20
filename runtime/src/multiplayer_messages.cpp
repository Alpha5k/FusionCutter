#include "multiplayer.hpp"

#include "core_runtime.hpp"
#include "message_protocol.hpp"
#include "patches/patch_record.hpp"

#include <fc/reporting.hpp>

#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <format>
#include <utility>

namespace fc::runtime {

std::optional<std::uint64_t> MultiplayerService::token() noexcept {
    std::uint64_t value{};
    do {
        if (BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(&value), sizeof(value), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) {
            return std::nullopt;
        }
    } while (!value);

    return value;
}

void MultiplayerService::fail_connection(Connection &connection, std::string_view reason) noexcept {
    const bool changed = connection.phase != Phase::Failed;
    connection.phase = Phase::Failed;
    connection.whole_notice = !connection.local;

    discard_messages(connection);

    try {
        connection.failure_reason = reason;
        if (changed) {
            log(FC_LOG_ERROR, std::format("Multiplayer connection failed for peer {} at native slot {} destination {}: {}",
                                          connection.id.value, connection.native_slot, connection.raw, reason));
        }
    } catch (...) {
        // The connection remains failed even if allocating its diagnostic text fails.
    }
}

void MultiplayerService::carrier_failed(std::string_view reason) {
    std::lock_guard lock(mutex_);
    expire_hellos(Clock::time_point::max());
    for (auto &connection : connections_) {
        if (connection.id.value && !connection.local) {
            fail_connection(connection, reason);
        }
    }
}

void MultiplayerService::expire_hellos(Clock::time_point now) {
    std::erase_if(pending_hellos_, [&](const PendingHello &hello) {
        if (now < hello.deadline) {
            return false;
        }
        incoming_bytes_ -= hello.charge();
        return true;
    });
}

void MultiplayerService::accept_hello(Connection &connection, std::uint64_t client_token, std::span<const std::string_view> manifest) {
    if (connection.phase != Phase::AwaitHello && connection.phase != Phase::Welcome && connection.phase != Phase::AwaitReady) {
        reject(connection, Rejection::WrongStage, "Hello arrived at the wrong negotiation stage");
        return;
    }
    if (connection.client_token == client_token) {
        reject(connection, Rejection::RepeatedHello, "Hello repeats the current connection token");
        return;
    }

    const auto random = token();
    if (!random) {
        fail_connection(connection, "Could not generate a multiplayer connection token");
        return;
    }

    connection.client_token = client_token;
    // A new Hello during negotiation receives a fresh host token; the old Ready cannot confirm it.
    connection.host_token = *random;
    for (auto &path : connection.paths) {
        path.remote_supported = std::ranges::binary_search(manifest, path.id);
    }
    connection.phase = Phase::Welcome;
    log(FC_LOG_DEBUG, std::format("Multiplayer peer {} at native slot {} destination {}: Hello received; submitting Welcome",
                                  connection.id.value, connection.native_slot, connection.raw));
}

void MultiplayerService::receive(bool server, std::uint64_t sender, Bytes bytes, Clock::time_point now) {
    const auto frame = message_protocol::parse(bytes);
    std::lock_guard lock(mutex_);
    if (!remote_available_) {
        return;
    }
    expire_hellos(now);

    // A client's host route may identify the lobby, while the incoming packet identifies that lobby's owner.
    const auto found = std::ranges::find_if(connections_, [&](const Connection &connection) {
        return connection.id.value && !connection.local &&
               (connection.raw == sender || (mode_ == GameMode::Client && connection.info.galaxy_id == sender));
    });
    if (found == connections_.end()) {
        // Galaxy can deliver Hello before the game reports this sender's native Connect.
        if (server && mode_ != GameMode::Client && frame && frame->kind == message_protocol::Kind::Hello) {
            const auto pending = std::ranges::find_if(pending_hellos_, [&](const PendingHello &hello) {
                return hello.sender == sender;
            });
            const auto old_charge = pending == pending_hellos_.end() ? 0 : pending->charge();
            const auto charge = sizeof(PendingHello) + bytes.size();
            if (charge <= kQueueBytes - (incoming_bytes_ - old_charge)) {
                try {
                    if (old_charge) {
                        // Keep the latest client attempt without extending the first Hello's deadline.
                        pending->bytes = std::vector<std::byte>{bytes.begin(), bytes.end()};
                    } else {
                        pending_hellos_.push_back({sender, now + std::chrono::seconds(3), {bytes.begin(), bytes.end()}});
                    }
                } catch (...) {
                    // Drop the Hello if allocating its retained storage fails.
                    return;
                }
                incoming_bytes_ = incoming_bytes_ - old_charge + charge;
                if (!old_charge) {
                    log(FC_LOG_DEBUG,
                        std::format("Multiplayer retained Hello from native sender {}; waiting for native connection registration",
                                    sender));
                }
                return;
            }
        }
        if (!unknown_sender_logged_ && frame &&
            (frame->kind == message_protocol::Kind::Hello || frame->kind == message_protocol::Kind::Welcome ||
             frame->kind == message_protocol::Kind::Ready)) {
            unknown_sender_logged_ = true;
            log(FC_LOG_DEBUG, std::format("Multiplayer negotiation rejected packet from native sender {}: unknown sender", sender));
        }
        return;
    }

    auto &connection = *found;
    if (!frame) {
        reject(connection, Rejection::InvalidPacket, "invalid packet");
        return;
    }
    if (server != (mode_ != GameMode::Client)) {
        reject(connection, Rejection::WrongRole, "wrong receive role");
        return;
    }
    timeout(connection, now);
    if (connection.phase != Phase::Established && now >= connection.deadline) {
        return;
    }

    using message_protocol::Kind;
    if (frame->kind == Kind::Hello && server) {
        accept_hello(connection, frame->tokens.client, frame->manifest);
        return;
    }

    if (frame->tokens.client != connection.client_token) {
        reject(connection, Rejection::ClientToken, "mismatched client connection token");
        return;
    }

    // Sending Hello releases the lock, so a concurrent receive can deliver Welcome before that send returns.
    if (frame->kind == Kind::Welcome && !server && (connection.phase == Phase::Hello || connection.phase == Phase::AwaitWelcome)) {
        connection.host_token = frame->tokens.host;
        for (auto &path : connection.paths) {
            path.remote_supported = std::ranges::binary_search(frame->manifest, path.id);
        }
        connection.phase = Phase::Ready;
        log(FC_LOG_DEBUG, std::format("Multiplayer peer {} at native slot {} destination {}: Welcome received; submitting Ready",
                                      connection.id.value, connection.native_slot, connection.raw));
        return;
    }

    if (frame->tokens.host != connection.host_token) {
        reject(connection, Rejection::HostToken, "mismatched host connection token");
        return;
    }

    if (frame->kind == Kind::Ready && server && (connection.phase == Phase::AwaitReady || connection.phase == Phase::Welcome)) {
        connection.phase = Phase::Established;
        log(FC_LOG_DEBUG, std::format("Multiplayer peer {} at native slot {} destination {}: Ready received; negotiation completed",
                                      connection.id.value, connection.native_slot, connection.raw));
        return;
    }
    if (frame->kind == Kind::Failed && connection.phase == Phase::Established) {
        if (frame->feature.empty()) {
            fail_connection(connection, "Peer reported a failed multiplayer connection");
            connection.whole_notice = false;
        } else {
            const auto path = std::ranges::find_if(connection.paths, [&](const FeaturePath &path) {
                return path.id == frame->feature;
            });
            if (path != connection.paths.end()) {
                if (!path->failed) {
                    path->reason = "Peer reported failed feature communication";
                    for (const auto &feature : features_) {
                        if (feature->id == path->id && feature->frozen && feature->owner->runtime) {
                            const auto peer = route(*feature, connection.id) == &connection ? connection.id : host_;
                            if (route(*feature, peer) == &connection) {
                                fc::Logger{{feature->owner, &feature->owner->runtime->host()}}.error(
                                    "Multiplayer delivery failed for peer {} feature '{}': {}", peer.value, feature->id, path->reason);
                            }
                        }
                    }
                }
                path->failed = true;
                path->notice_pending = false;
            }
        }

        return;
    }

    if (frame->kind != Kind::Data || (connection.phase != Phase::Established && connection.phase != Phase::Ready)) {
        reject(connection, Rejection::WrongStage, "packet arrived at the wrong role or negotiation stage");
        return;
    }

    // A reply can arrive while Ready is being sent. Queue it now; delivery waits until the send succeeds.
    auto *feature = implementation(frame->feature, server ? FC_FEATURE_HOST : FC_FEATURE_CLIENT);
    const PeerId peer = server ? connection.id : host_;
    const auto path = std::ranges::find(connection.paths, frame->feature, &FeaturePath::id);
    if (!feature || path == connection.paths.end() || path->failed || !path->remote_supported) {
        return;
    }

    const auto charge = sizeof(IncomingMessage) + frame->payload.size();
    if (charge > kQueueBytes - incoming_bytes_) {
        fail_connection(connection, "Multiplayer incoming queue is full");
        return;
    }

    try {
        incoming_.push_back({feature, peer, connection.id, {frame->payload.begin(), frame->payload.end()}});
        incoming_bytes_ += charge;
    } catch (...) {
        fail_connection(connection, "Could not queue an incoming multiplayer message");
    }
}

void MultiplayerService::pump(Carrier carrier, Clock::time_point now) {
    std::unique_lock lock(mutex_);
    expire_hellos(now);
    if (pumping_) {
        return;
    }
    pumping_ = true;

    // Prevent nested or concurrent pumps while a native send has released the mutex.
    struct Release {
        bool &flag;

        // Allows the next pump after this pass ends.
        ~Release() {
            flag = false;
        }
    } release{pumping_};

    // Copy IDs because native sends may change the connection list while the mutex is released.
    std::size_t submitted{};
    std::vector<PeerId> peers;
    for (auto &connection : connections_) {
        if (!connection.id.value) {
            continue;
        }
        timeout(connection, now);
        peers.push_back(connection.id);
    }

    for (const auto peer : peers) {
        while (submitted < 16) {
            auto found = std::ranges::find_if(connections_, [&](const Connection &connection) {
                return connection.id == peer;
            });
            if (found == connections_.end()) {
                break;
            }

            auto &connection = *found;
            if (connection.local) {
                if (connection.outgoing.empty()) {
                    break;
                }

                // Self-host messages use the same incoming queue and deferred callbacks as remote messages.
                auto message = std::move(connection.outgoing.front());
                connection.outgoing.pop_front();
                outgoing_bytes_ -= message.charge();
                const bool to_host = message.peer == host_;
                auto *recipient = implementation(message.feature->id, to_host ? FC_FEATURE_HOST : FC_FEATURE_CLIENT);
                if (recipient && agreement_locked(*message.feature, message.peer) == Agreement::Compatible) {
                    const auto charge = sizeof(IncomingMessage) + message.bytes.size();
                    if (charge > kQueueBytes - incoming_bytes_) {
                        fail_connection(connection, "Multiplayer incoming queue is full");
                    } else {
                        incoming_bytes_ += charge;
                        try {
                            incoming_.push_back({recipient, to_host ? local_ : host_, connection.id, std::move(message.bytes)});
                        } catch (...) {
                            incoming_bytes_ -= charge;
                            fail_connection(connection, "Could not queue a local multiplayer message");
                        }
                    }
                }

                ++submitted;
                continue;
            }

            if (!remote_available_ || !carrier.submit || connection.phase == Phase::Expired) {
                break;
            }

            // Send handshake and failure notices before ordinary data for this connection.
            std::array<std::byte, message_protocol::kFrameBytes> bytes{};
            const message_protocol::Tokens tokens{connection.client_token, connection.host_token};
            const auto phase = connection.phase;
            std::size_t size{};
            FeaturePath *notice{};
            bool data{}, whole{};
            FC_Feature *data_feature{};
            using message_protocol::Kind;
            if (phase == Phase::Hello) {
                size = message_protocol::encode_manifest(Kind::Hello, tokens, client_manifest_, bytes);
            } else if (phase == Phase::Welcome) {
                size = message_protocol::encode_manifest(Kind::Welcome, tokens, host_manifest_, bytes);
            } else if (phase == Phase::Ready) {
                size = message_protocol::encode_ready(tokens, bytes);
            } else if (phase == Phase::Established || phase == Phase::Failed) {
                if (connection.whole_notice) {
                    whole = true;
                    size = message_protocol::encode_failed(tokens, {}, bytes);
                } else {
                    const auto pending = std::ranges::find_if(connection.paths, [](const FeaturePath &path) {
                        return path.notice_pending;
                    });
                    if (pending != connection.paths.end()) {
                        notice = &*pending;
                        size = message_protocol::encode_failed(tokens, notice->id, bytes);
                    } else if (phase == Phase::Established && !connection.outgoing.empty()) {
                        const auto &message = connection.outgoing.front();
                        if (agreement_locked(*message.feature, message.peer) != Agreement::Compatible) {
                            outgoing_bytes_ -= message.charge();
                            connection.outgoing.pop_front();
                            continue;
                        }
                        data = true;
                        data_feature = message.feature;
                        size = message_protocol::encode_data(tokens, message.feature->id, message.bytes, bytes);
                    }
                }
            }
            if (!size) {
                break;
            }

            const std::string_view notice_id = notice ? notice->id : std::string_view{};
            const auto raw = connection.raw;
            const bool server = mode_ != GameMode::Client;
            ++submitted;
            lock.unlock();
            const auto result = carrier.submit(carrier.context, server, raw, Bytes{bytes.data(), size});
            lock.lock();

            // Recheck the connection and handshake tokens after native callbacks had a chance to replace them.
            found = std::ranges::find_if(connections_, [&](const Connection &current) {
                return current.id == peer;
            });
            if (found == connections_.end() || found->client_token != tokens.client) {
                break;
            }

            // Welcome can set the host token during Hello submission; that valid receive does not undo native acceptance.
            auto &current = *found;
            const bool tokens_match = current.host_token == tokens.host;
            const bool reentrant_hello = phase == Phase::Hello && !tokens.host && current.host_token && current.phase == Phase::Ready;
            const bool handshake = phase == Phase::Hello || phase == Phase::Welcome || phase == Phase::Ready;
            if (result == CarrierResult::Accepted && handshake && current.phase != Phase::Failed && current.phase != Phase::Expired &&
                (tokens_match || reentrant_hello)) {
                const auto frame_name = phase == Phase::Hello ? "Hello" : phase == Phase::Welcome ? "Welcome" : "Ready";
                log(FC_LOG_DEBUG, std::format("Multiplayer peer {} at native slot {} destination {}: {} submission accepted{}",
                                              current.id.value, current.native_slot, current.raw, frame_name,
                                              phase == Phase::Ready && current.phase == phase ? "; negotiation completed" : ""));
            }
            if (!tokens_match) {
                break;
            }

            if (result == CarrierResult::Retry) {
                break;
            }
            if (result == CarrierResult::Failed) {
                fail_connection(current, "Multiplayer transport rejected a packet");
                current.whole_notice = false;
                break;
            }

            if (phase == Phase::Hello && current.phase == phase) {
                current.phase = Phase::AwaitWelcome;
            } else if (phase == Phase::Welcome && current.phase == phase) {
                current.phase = Phase::AwaitReady;
            } else if (phase == Phase::Ready && current.phase == phase) {
                current.phase = Phase::Established;
            } else if (whole) {
                current.whole_notice = false;
            } else if (!notice_id.empty()) {
                for (auto &path : current.paths) {
                    if (path.id == notice_id) {
                        path.notice_pending = false;
                    }
                }
            } else if (data && !current.outgoing.empty() && current.outgoing.front().feature == data_feature) {
                outgoing_bytes_ -= current.outgoing.front().charge();
                current.outgoing.pop_front();
            }
        }
    }
}

} // namespace fc::runtime
