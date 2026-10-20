#include "service.hpp"

#include "protocol.hpp"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <mutex>
#include <string_view>
#include <utility>
#include <vector>

namespace fc::core_plugin::lua_multiplayer {
namespace {

// Allocates a registration ID unique within this process; canceled proposals also consume their ID.
std::uint64_t next_registration() noexcept {
    static std::atomic<std::uint64_t> next{};
    const auto previous = next.fetch_add(1, std::memory_order_relaxed);
    assert(previous != std::numeric_limits<std::uint64_t>::max());
    return previous + 1;
}

} // namespace

fc::Result<> Service::prepare(fc::PrepareContext &context) {
    game_ = context.game();
    multiplayer_ = context.multiplayer();
    logger_ = context.log();
    auto bindings = fc::lua::get(context);
    if (!bindings) {
        return std::unexpected(bindings.error());
    }
    bindings_ = std::move(*bindings);

    auto feature = multiplayer_.feature({"FC.LuaMessages", fc::FeatureRole::Both});
    if (!feature) {
        return std::unexpected(feature.error());
    }
    feature_ = *feature;
    max_payload_ = feature_.max_payload();

    // Begin with an empty complete list so peers can learn that this service has no listeners yet.
    auto initial = protocol::encode_listeners({}, max_payload_);
    if (!initial) {
        return std::unexpected(initial.error());
    }
    announcement_ = std::make_shared<const std::vector<std::byte>>(std::move(*initial));

    // Runtime notifications and receive callbacks are serialized on the control thread; Lua calls may overlap them.
    if (auto result = bindings_.on_state([this](fc::lua::StateId state, bool alive) noexcept {
            state_changed(state, alive);
        });
        !result) {
        return result;
    }
    if (auto result = feature_.on_peer([this](fc::PeerId peer, fc::Agreement agreement) noexcept {
            peer_changed(peer, agreement);
        });
        !result) {
        return result;
    }
    if (auto result = feature_.on_message([this](fc::PeerId peer, fc::Bytes bytes) noexcept {
            receive(peer, bytes);
        });
        !result) {
        return result;
    }
    return register_script(bindings_);
}

fc::Result<std::shared_ptr<const std::vector<std::byte>>> Service::announcement(std::string_view channel,
                                                                                const std::shared_ptr<Listener> &replacement) const {
    std::vector<protocol::Listener> list;
    list.reserve(listeners_.size() + (replacement ? 1 : 0));

    // Preserve other channels' registrations while assembling the proposed replacement or removal.
    for (const auto &[name, listener] : listeners_) {
        if (name != channel) {
            list.push_back({listener->registration, name});
        }
    }
    if (replacement) {
        list.push_back({replacement->registration, channel});
    }

    auto bytes = protocol::encode_listeners(list, max_payload_);
    if (!bytes) {
        return std::unexpected(bytes.error());
    }
    return std::make_shared<const std::vector<std::byte>>(std::move(*bytes));
}

fc::Result<> Service::listen(fc::lua::Frame &frame, std::string_view channel, fc::lua::Value function) {
    if (!protocol::valid_channel(channel)) {
        return std::unexpected(fc::Error{"Expected a nonempty UTF-8 channel name"});
    }

    // Retain before locking so Lua's registry work runs outside the service mutex.
    auto reference = frame.retain(function);
    if (!reference) {
        return std::unexpected(reference.error());
    }
    auto replacement = std::make_shared<Listener>(next_registration(), std::move(*reference));
    std::shared_ptr<Listener> retired;
    {
        std::lock_guard lock{mutex_};
        const auto found = listeners_.find(channel);
        // A channel belongs to one live VM; reference validity checks provider records rather than executing Lua.
        if (found != listeners_.end() && found->second->function.state() != frame.state() && found->second->function) {
            return std::unexpected(fc::Error{"Lua channel belongs to another live VM"});
        }
        auto proposed = announcement(channel, replacement);
        if (!proposed) {
            return std::unexpected(fc::Error{"Lua listener list exceeds the feature payload limit"});
        }

        // Replace the handler and complete announcement together; release the old reference after unlocking.
        if (found != listeners_.end()) {
            retired = std::exchange(found->second, std::move(replacement));
        } else {
            listeners_.emplace(channel, std::move(replacement));
        }
        announcement_ = std::move(*proposed);
    }
    return {};
}

fc::Result<> Service::close(fc::lua::StateId state, std::string_view channel) {
    if (!protocol::valid_channel(channel)) {
        return std::unexpected(fc::Error{"Expected a nonempty UTF-8 channel name"});
    }

    std::shared_ptr<Listener> retired;
    {
        std::lock_guard lock{mutex_};
        const auto found = listeners_.find(channel);
        if (found == listeners_.end()) {
            return {};
        }
        if (found->second->function.state() != state) {
            return std::unexpected(fc::Error{"Lua channel belongs to another VM"});
        }
        auto proposed = announcement(channel, {});
        if (!proposed) {
            return std::unexpected(proposed.error());
        }

        // Publish the list without this channel; keep its function alive until the mutex is released.
        retired = std::move(found->second);
        listeners_.erase(found);
        announcement_ = std::move(*proposed);
    }
    return {};
}

std::shared_ptr<fc::lua::FunctionRef> Service::peer_cache(fc::lua::StateId state) const {
    std::lock_guard lock{mutex_};
    const auto found = peer_caches_.find(state.value);
    return found == peer_caches_.end() ? nullptr : found->second;
}

void Service::set_peer_cache(fc::lua::StateId state, std::shared_ptr<fc::lua::FunctionRef> function) {
    std::shared_ptr<fc::lua::FunctionRef> retired;
    {
        std::lock_guard lock{mutex_};
        retired = std::exchange(peer_caches_[state.value], std::move(function));
    }
}

void Service::state_changed(fc::lua::StateId state, bool alive) {
    if (alive) {
        return;
    }

    // Release removed functions after unlocking because their cleanup accesses the Lua provider's records.
    std::vector<std::shared_ptr<Listener>> retired;
    std::shared_ptr<fc::lua::FunctionRef> cache;
    {
        std::lock_guard lock{mutex_};
        for (auto iterator = listeners_.begin(); iterator != listeners_.end();) {
            if (iterator->second->function.state() == state) {
                retired.push_back(std::move(iterator->second));
                iterator = listeners_.erase(iterator);
            } else {
                ++iterator;
            }
        }

        const auto found = peer_caches_.find(state.value);
        if (found != peer_caches_.end()) {
            cache = std::move(found->second);
            peer_caches_.erase(found);
        }

        // Rebuild the complete announcement after removing every listener owned by the ended VM.
        if (!retired.empty()) {
            auto current = announcement({}, {});
            if (current) {
                announcement_ = std::move(*current);
            }
        }
    }
}

void Service::peer_changed(fc::PeerId id, fc::Agreement agreement) {
    std::lock_guard lock{mutex_};
    const auto found = std::ranges::find(peers_, id, &Peer::id);
    if (agreement == fc::Agreement::Unavailable) {
        if (found != peers_.end()) {
            peers_.erase(found);
        }
        return;
    }
    if (found == peers_.end()) {
        peers_.push_back({id, agreement});
    } else {
        found->agreement = agreement;
        // Listener registrations belong to a compatible connection and expire when compatibility is lost.
        if (agreement != fc::Agreement::Compatible) {
            found->listeners.clear();
            found->announced.reset();
        }
    }
}

void Service::update() {
    // Read APIs restricted to the control thread before publishing observations for Lua queries.
    View current{game_.current(), multiplayer_.host(), multiplayer_.local(), multiplayer_.clients()};
    std::vector<fc::PeerId> destinations;
    std::shared_ptr<const std::vector<std::byte>> latest;
    {
        std::lock_guard lock{mutex_};
        view_ = std::move(current);

        // Peer notifications maintain compatibility; retry each connection's latest unsent announcement.
        for (const auto &peer : peers_) {
            if (peer.agreement == fc::Agreement::Compatible && peer.announced != announcement_) {
                destinations.push_back(peer.id);
            }
        }
        latest = announcement_;
    }

    // Accepted older lists remain ordered in the native queue; a concurrent listener change waits for the next update.
    for (const auto id : destinations) {
        if (feature_.send(id, *latest) != fc::SendResult::Accepted) {
            continue;
        }
        std::lock_guard lock{mutex_};
        const auto found = std::ranges::find(peers_, id, &Peer::id);
        if (found != peers_.end()) {
            found->announced = latest;
        }
    }
}

Service::View Service::snapshot() const {
    std::lock_guard lock{mutex_};
    return view_;
}

bool Service::ready(fc::PeerId peer, std::string_view channel) const {
    std::lock_guard lock{mutex_};
    const auto found = std::ranges::find(peers_, peer, &Peer::id);
    return found != peers_.end() && found->agreement == fc::Agreement::Compatible && found->listeners.contains(channel);
}

} // namespace fc::core_plugin::lua_multiplayer
