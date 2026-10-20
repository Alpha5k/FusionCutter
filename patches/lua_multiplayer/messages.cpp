#include "service.hpp"

#include "protocol.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <new>
#include <string_view>
#include <utility>
#include <vector>

namespace fc::core_plugin::lua_multiplayer {
namespace {

// This 64 KiB limit counts delivery records and vector capacity held by deferred Lua calls.
constexpr std::size_t kRetainedBytes = 64 * 1024;

// Maps the native send result to the script contract's exact result text.
std::string_view result_text(fc::SendResult result) noexcept {
    switch (result) {
    case fc::SendResult::Accepted:
        return "accepted";
    case fc::SendResult::Unavailable:
        return "unavailable";
    case fc::SendResult::Pending:
        return "pending";
    case fc::SendResult::Incompatible:
        return "incompatible";
    case fc::SendResult::Full:
        return "full";
    case fc::SendResult::TooLarge:
        return "too_large";
    case fc::SendResult::Failed:
        return "failed";
    }
    return "unavailable";
}

// Maps a value encoding failure to the script contract's exact result text.
std::string_view encoding_text(protocol::EncodeError error) noexcept {
    return error == protocol::EncodeError::InvalidValue ? "invalid_value" : "too_large";
}

} // namespace

// Keeps the selected listener and copied payload alive until the deferred Lua call is cleaned up.
struct Service::Delivery {
    std::shared_ptr<Listener> listener;
    fc::PeerId peer;
    std::vector<std::byte> value;
    // Nonnull only after admission; payload capacity stays unchanged until this counter is released.
    std::shared_ptr<std::atomic<std::size_t>> counter;

    // Returns the charged bytes when the final reference is released; accepted calls are cleaned up on the control thread.
    ~Delivery() {
        if (counter) {
            counter->fetch_sub(sizeof(Delivery) + value.capacity(), std::memory_order_relaxed);
        }
    }
};

std::string_view Service::send(fc::PeerId peer, std::string_view channel, fc::Bytes value) {
    // A Lua call can observe a feature failure before the control thread refreshes the copied compatibility.
    switch (feature_.agreement(peer)) {
    case fc::Agreement::Unavailable:
        return "unavailable";
    case fc::Agreement::Pending:
        return "pending";
    case fc::Agreement::Incompatible:
        return "incompatible";
    case fc::Agreement::Failed:
        return "failed";
    case fc::Agreement::Compatible:
        break;
    }

    // Copy the advertised registration under the mutex, then encode and submit outside it.
    std::uint64_t registration{};
    {
        std::lock_guard lock{mutex_};
        const auto found = std::ranges::find(peers_, peer, &Peer::id);
        if (found == peers_.end()) {
            return "unavailable";
        }
        const auto listener = found->listeners.find(channel);
        if (listener == found->listeners.end()) {
            return "no_listener";
        }
        registration = listener->second;
    }

    auto packet = protocol::encode_message(registration, value, max_payload_);
    if (!packet) {
        return "too_large";
    }
    return result_text(feature_.send(peer, *packet));
}

std::string_view Service::send_host(std::string_view channel, fc::lua::Value value) {
    const auto current = snapshot();
    if (!current.state || !current.host.value || (!current.state->is_client() && !(current.state->is_host() && current.local.value))) {
        return "unavailable";
    }
    if (max_payload_ < protocol::kMessageHeaderBytes) {
        return "too_large";
    }

    // Reserve part of the feature payload for the destination registration and packet kind.
    auto encoded = protocol::encode_value(value, max_payload_ - protocol::kMessageHeaderBytes);
    return encoded ? send(current.host, channel, *encoded) : encoding_text(encoded.error());
}

std::string_view Service::send_client(fc::PeerId peer, std::string_view channel, fc::lua::Value value) {
    const auto current = snapshot();
    if (!current.state || !current.state->is_host() || std::ranges::find(current.clients, peer) == current.clients.end()) {
        return "unavailable";
    }
    if (max_payload_ < protocol::kMessageHeaderBytes) {
        return "too_large";
    }

    auto encoded = protocol::encode_value(value, max_payload_ - protocol::kMessageHeaderBytes);
    return encoded ? send(peer, channel, *encoded) : encoding_text(encoded.error());
}

bool Service::broadcast(std::string_view channel, fc::lua::Value value) {
    const auto current = snapshot();
    if (!current.state || !current.state->is_host() || !current.host.value || max_payload_ < protocol::kMessageHeaderBytes) {
        return false;
    }

    // Every destination shares this value encoding; send adds that peer's advertised registration ID.
    auto encoded = protocol::encode_value(value, max_payload_ - protocol::kMessageHeaderBytes);
    if (!encoded) {
        return false;
    }

    bool accepted = true;
    for (const auto peer : current.clients) {
        if (ready(peer, channel) && send(peer, channel, *encoded) != "accepted") {
            accepted = false;
        }
    }
    return accepted;
}

bool Service::current(const Delivery &delivery) const {
    // Pointer equality ties the delivery to its original listener even if another now uses the same channel.
    {
        std::lock_guard lock{mutex_};
        const auto peer = std::ranges::find(peers_, delivery.peer, &Peer::id);
        if (peer == peers_.end() || peer->agreement != fc::Agreement::Compatible ||
            std::ranges::none_of(listeners_, [&](const auto &entry) {
                return entry.second == delivery.listener;
            })) {
            return false;
        }
    }

    // Provider and runtime checks observe VM expiry or feature failure before the copied state is refreshed.
    return delivery.listener->function && feature_.agreement(delivery.peer) == fc::Agreement::Compatible;
}

void Service::receive(fc::PeerId peer, fc::Bytes bytes) {
    try {
        if (bytes.empty()) {
            feature_.fail(peer, "Lua packet is empty");
            return;
        }

        const auto kind = std::to_integer<unsigned>(bytes.front());
        if (kind == 1) {
            auto decoded = protocol::decode_listeners(bytes);
            if (!decoded) {
                feature_.fail(peer, decoded.error().message);
                return;
            }

            // Replace the prior listener list only after the complete announcement has passed validation.
            std::lock_guard lock{mutex_};
            const auto found = std::ranges::find(peers_, peer, &Peer::id);
            if (found != peers_.end() && found->agreement == fc::Agreement::Compatible) {
                found->listeners = std::move(*decoded);
            }
            return;
        }

        if (kind != 2) {
            feature_.fail(peer, "Lua packet has an unsupported kind");
            return;
        }

        auto decoded = protocol::decode_message(bytes);
        if (!decoded) {
            feature_.fail(peer, decoded.error().message);
            return;
        }

        // Route by registration ID to bind queued packets to the listener that was advertised.
        std::shared_ptr<Listener> listener;
        {
            std::lock_guard lock{mutex_};
            const auto found = std::ranges::find_if(listeners_, [&](const auto &entry) {
                return entry.second->registration == decoded->destination;
            });
            if (found == listeners_.end()) {
                return;
            }
            listener = found->second;
        }

        auto delivery = std::make_shared<Delivery>();
        delivery->listener = std::move(listener);
        delivery->peer = peer;
        if (!current(*delivery)) {
            return;
        }

        // Only the control thread adds retained bytes; cleanup can only lower this observed byte count.
        const auto used = retained_bytes_->load(std::memory_order_relaxed);
        if (sizeof(Delivery) + decoded->value.size() > kRetainedBytes - used) {
            feature_.fail(peer, "Retaining this Lua message would exceed the 64 KiB storage limit");
            return;
        }

        // Charge allocated capacity, including spare vector storage, as well as the delivery record.
        delivery->value.assign(decoded->value.begin(), decoded->value.end());
        const auto charge = sizeof(Delivery) + delivery->value.capacity();
        if (charge > kRetainedBytes - used) {
            feature_.fail(peer, "Retaining this Lua message would exceed the 64 KiB storage limit");
            return;
        }
        delivery->counter = retained_bytes_;
        retained_bytes_->fetch_add(charge, std::memory_order_relaxed);

        const auto result = bindings_.defer(delivery->listener->function.state(), [this, delivery](fc::lua::Frame &frame) {
            deliver(frame, delivery);
        });
        // An ended VM, replaced listener or ended connection cancels delivery; refusing current work loses reliable data.
        if (result != fc::lua::DispatchResult::Accepted && current(*delivery)) {
            feature_.fail(peer, "Could not queue a reliable Lua message for its listener");
        }
    } catch (const std::bad_alloc &) {
        feature_.fail(peer, "Could not allocate storage for a Lua message");
    }
}

void Service::deliver(fc::lua::Frame &frame, const std::shared_ptr<Delivery> &delivery) {
    if (frame.state() != delivery->listener->function.state() || !current(*delivery)) {
        return;
    }

    auto value = protocol::decode_value(frame, delivery->value);
    if (!value) {
        feature_.fail(delivery->peer, value.error().message);
        return;
    }

    auto sender = peer_value(frame, delivery->peer);
    // Decoding and creating the sender userdata use Lua; recheck routing immediately before invoking the handler.
    if (sender.type() != fc::lua::Type::Userdata || !current(*delivery)) {
        return;
    }

    // A handler error is logged; packet or storage failures above mark the peer's messaging feature Failed.
    auto called = frame.call(delivery->listener->function, {sender, *value});
    if (!called) {
        logger_.error("Lua listener {} failed for peer {} in state {}: {}", delivery->listener->registration, delivery->peer.value,
                      frame.state().value, called.error().message);
    }
}

} // namespace fc::core_plugin::lua_multiplayer
