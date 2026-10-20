#pragma once

#include <fc/contexts.hpp>
#include <fc/game.hpp>
#include <fc/lua.hpp>
#include <fc/multiplayer.hpp>
#include <fc/reporting.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fc::core_plugin::lua_multiplayer {

// Owns Lua channel listeners and announces their registration IDs to multiplayer peers.
class Service {
  public:
    // Registers the feature, Lua declarations and callbacks during prepare().
    fc::Result<> prepare(fc::PrepareContext &context);

    // Copies current peer observations and retries the latest unsent announcements on the control thread.
    void update();

  private:
    // Stores observations copied from the control thread for script queries on the owning Lua thread.
    struct View {
        std::optional<fc::GameState> state;
        fc::PeerId host, local;
        std::vector<fc::PeerId> clients;
    };

    // Retains a handler in its VM; replaces a listener only when the complete announcement fits.
    fc::Result<> listen(fc::lua::Frame &frame, std::string_view channel, fc::lua::Value function);

    // Removes the calling VM's listener; closing an absent channel succeeds.
    fc::Result<> close(fc::lua::StateId state, std::string_view channel);

    // Sends to the copied current host through a remote client's endpoint or the host's local client endpoint.
    // Returns a send or encoding result as text.
    std::string_view send_host(std::string_view channel, fc::lua::Value value);

    // Sends to a current client while hosting and returns a send or encoding result as text.
    std::string_view send_client(fc::PeerId peer, std::string_view channel, fc::lua::Value value);

    // Encodes once and attempts every ready client; returns false if unavailable, encoding fails or any send is refused.
    // An empty set of ready clients succeeds.
    bool broadcast(std::string_view channel, fc::lua::Value value);

    // Returns copied game and peer observations for script queries on the Lua thread.
    View snapshot() const;

    // Checks copied compatibility and the latest listener list received from this peer.
    bool ready(fc::PeerId peer, std::string_view channel) const;

    // Owns a retained handler and the registration ID that routes incoming packets to it.
    struct Listener {
        std::uint64_t registration;
        fc::lua::FunctionRef function;
    };

    // Stores a peer's compatibility and latest complete listener list received from that peer.
    struct Peer {
        fc::PeerId id;
        fc::Agreement agreement;
        std::map<std::string, std::uint64_t, std::less<>> listeners;
        // Last complete local listener list accepted by the native queue for this connection.
        std::shared_ptr<const std::vector<std::byte>> announced;
    };

    struct Delivery;

    // Registers the private initializer for the peer cache and the ten script functions during prepare().
    fc::Result<> register_script(fc::lua::Bindings &bindings);

    // Retains a Lua cache function with weak values in each VM before its script bindings install.
    void initialize_peer_cache(fc::lua::Frame &frame);

    // Reuses this VM's cached opaque userdata for the full peer identity.
    fc::lua::Value peer_value(fc::lua::Frame &frame, fc::PeerId peer);

    // Accepts only tagged userdata containing one complete nonzero peer identity.
    static std::optional<fc::PeerId> read_peer(fc::lua::Value value);

    // Keeps the cache function alive during Lua use by copying its shared ownership.
    std::shared_ptr<fc::lua::FunctionRef> peer_cache(fc::lua::StateId state) const;

    // Replaces the retained cache function, releasing its previous reference after unlocking.
    void set_peer_cache(fc::lua::StateId state, std::shared_ptr<fc::lua::FunctionRef> function);

    // Removes listener routing and cached peer functions after the owning VM ends.
    void state_changed(fc::lua::StateId state, bool alive);

    // Updates compatibility on the control thread and discards listener data when compatibility is lost.
    void peer_changed(fc::PeerId peer, fc::Agreement agreement);

    // Processes packets in native order and schedules eligible deliveries through the existing Lua queue.
    void receive(fc::PeerId peer, fc::Bytes bytes);

    // Rechecks the destination, decodes its value and invokes the retained handler on its Lua thread.
    void deliver(fc::lua::Frame &frame, const std::shared_ptr<Delivery> &delivery);

    // Checks that the same listener is still installed, its VM is live and the peer is compatible.
    bool current(const Delivery &delivery) const;

    // Encodes a complete proposed listener list; the caller holds mutex_.
    fc::Result<std::shared_ptr<const std::vector<std::byte>>> announcement(std::string_view channel,
                                                                           const std::shared_ptr<Listener> &replacement) const;

    // Checks current compatibility, copies the advertised registration ID and sends after releasing mutex_.
    std::string_view send(fc::PeerId peer, std::string_view channel, fc::Bytes value);

    // Protects shared listener and peer state; send messages and execute Lua after unlocking.
    mutable std::mutex mutex_;
    fc::Game game_;
    fc::Multiplayer multiplayer_;
    fc::Feature feature_;
    fc::lua::Bindings bindings_;
    fc::Logger logger_;
    std::size_t max_payload_{};
    View view_;
    std::map<std::string, std::shared_ptr<Listener>, std::less<>> listeners_;
    std::map<std::uint64_t, std::shared_ptr<fc::lua::FunctionRef>> peer_caches_;
    std::vector<Peer> peers_;
    std::shared_ptr<const std::vector<std::byte>> announcement_;

    // Counts Delivery record bytes and payload capacity retained for queued Lua calls.
    // Accepted captures keep this counter alive until the control thread releases their storage.
    std::shared_ptr<std::atomic<std::size_t>> retained_bytes_ = std::make_shared<std::atomic<std::size_t>>(0);
};

} // namespace fc::core_plugin::lua_multiplayer
