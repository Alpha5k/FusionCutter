# Multiplayer features and messages

The multiplayer service lets patches exchange custom data between a host and its connected clients. A
feature might send a request to the host, distribute an extra setting or exchange diagnostic information.
FC supplies peer identities, checks whether both endpoints advertise matching support and queues messages
through the supported game's transport. The patch defines what those messages mean.

An FC **feature** is a named protocol registration, such as `Example.Ping`, owned by a patch. It distinguishes
that patch's traffic and compatibility from other extensions using the same connection. A patch can declare
more than one feature when it implements separate protocols. A patch ID controls installation and configuration;
a feature ID tells the remote side which communication behavior is available.

Include `<fc/multiplayer.hpp>`. Get `Multiplayer` from `PrepareContext::multiplayer()`, register a feature
during Prepare and retain its `Feature` handle. The current backend uses Galaxy; see the Multiplayer messaging
row in the [build support tables](../installation.md#feature-support). Declaration compatibility and actual
session availability are separate checks.

## What feature agreement establishes

Having the same game build does not mean two players installed the same extensions. FC exchanges feature
support for the session. A feature becomes Compatible when the applicable endpoints advertise the matching
ID. Until that check completes, agreement is Pending; if matching support is missing, it is Incompatible.

The matching ID is the authors' statement that both implementations understand the protocol. FC does not
inspect payload fields to discover whether their meaning changed. If one release sends a different format,
use a different feature ID or retain a protocol that both implementations can understand. Also validate
incoming data according to the feature's rules before using it to change game state.

Negotiation is useful for optional extensions: a patch can leave its extra behavior inactive for a peer
that lacks it. Declaring a feature does not by itself require every player to install it, identify a player
object in game memory, or decide the server's policy for clients without support.

## Peers and roles

A peer is a communication endpoint in the current session. The host endpoint receives requests; client
endpoints receive host messages. A playing host fills both roles, while a dedicated server has no local
player client. Keeping those endpoints distinct allows the same request/reply code to work for a local
hosted player and for a player on another machine.

`PeerId` contains a process-local `uint64_t value`; zero means absent. IDs support equality and identify a
connection lifetime. They are not player indices or portable network identities.

| Method | Meaning |
| --- | --- |
| `Multiplayer::host()` | Host endpoint, including this process when hosting |
| `Multiplayer::local()` | This process's local client; absent on a dedicated server |
| `Multiplayer::clients()` | Copy the host's clients on the control thread, including its local client; empty on a remote client |
| `Multiplayer::info(peer)` | Optional copied PeerInfo; unavailable or expired peers return nullopt |
| `Multiplayer::feature(definition)` | Register an exact feature ID and roles during Prepare; returns `Result<Feature>` |

A playing host has distinct host and local-client endpoints. Clients send to the host; hosts send to their
clients. Local delivery follows the same message queue as remote delivery. A remote client does not get
direct peer-to-peer routing through this interface.

`PeerInfo::slot` is an optional game connection slot, not necessarily a player index; zero is valid.
`galaxy_id` is the optional Galaxy user ID. Keeping a PeerInfo copy does not keep the connection alive.

`FeatureDefinition` contains `id` and `role`, defaulting to `FeatureRole::Both`. Roles are Client, Host
(including dedicated servers), and Both. They describe this feature's implemented protocol roles, not
the launch Role or GameMode.

Feature IDs are exact and case-sensitive. Use a stable name for compatible behavior and change the ID when
the protocol or behavior becomes incompatible. Core features use names such as `FC.LuaMessages`; use your
own namespace for external features. A feature's support is frozen at Game Start from activated patches.
A patch activating later participates in the next session.

For a protocol implemented identically by clients and hosts, use Both. When different patches implement
the two halves, register the matching feature ID with the appropriate role in each. `FeatureRole` describes
protocol participation; `Role::Client` in a variant still includes a player who hosts a game.

Feature IDs must be nonempty UTF-8. Across all patches in one process, an ID can have only one registration
for each role: separate Client and Host registrations may share an ID, but Both overlaps either one.
An invalid definition, overlapping role or registration outside Prepare returns an error.

Registration success does not prove an active connection exists. A default Multiplayer handle returns
zero host/local IDs, an empty client list and `nullopt` peer information; feature registration returns an
error. With a valid handle, those same empty query results can simply mean no applicable session or peer.

## Register callbacks and send

This handler sends a one-byte request when a compatible host becomes available. The recipient sends a
different byte back, avoiding a reply loop even when both endpoints are local:

```cpp
#include <fc/plugin.hpp>

class PingHandler {
  public:
    // Registers the protocol and its control-thread callbacks before activation.
    fc::Result<> prepare(fc::PrepareContext &context) {
        multiplayer_ = context.multiplayer();
        log_ = context.log();
        auto feature = multiplayer_.feature({"Example.Ping", fc::FeatureRole::Both});
        if (!feature) {
            return std::unexpected(feature.error());
        }
        feature_ = *feature;

        auto messages = feature_.on_message([this](fc::PeerId peer, fc::Bytes bytes) noexcept {
            if (bytes.size() != 1) {
                feature_.fail(peer, "Ping message must contain one byte");
                return;
            }
            if (bytes[0] == std::byte{0}) {
                const auto reply = fc::byte_array<1>();
                const auto result = feature_.send(peer, reply);
                if (result != fc::SendResult::Accepted) {
                    log_.debug("Ping reply was not queued");
                }
            } else if (bytes[0] == std::byte{1}) {
                log_.debug("Ping reply received");
            } else {
                feature_.fail(peer, "Ping message has an unknown value");
            }
        });
        if (!messages) {
            return messages;
        }
        return feature_.on_peer([this](fc::PeerId peer, fc::Agreement agreement) noexcept {
            if (peer == multiplayer_.host() && agreement == fc::Agreement::Compatible) {
                const auto request = fc::byte_array<0>();
                if (feature_.send(peer, request) != fc::SendResult::Accepted) {
                    log_.debug("Ping request was not queued");
                }
            }
        });
    }

  private:
    fc::Multiplayer multiplayer_;
    fc::Feature feature_;
    fc::Logger log_;
};
```

Declare its variant for `BF2.Steam.59EDE353` and `BF2.GOG.59EDF52B`. For an actual protocol, define payload
encoding and validation explicitly; do not send pointer-containing C++ objects as wire data.

The example sends one byte per message: value 0 requests a reply and value 1 is the reply. Prepare registers
both callbacks. After activation and feature agreement, `on_peer` notices a compatible host and
queues the request. The receiving `on_message` callback checks the bytes and sends the reply to the sender.
The original sender then logs `Ping reply received` at Debug level.

Both processes need the matching feature installed before the session starts. A successful Prepare alone
does not cause a ping: there must also be an applicable connection and Compatible agreement. This makes
the example useful for separating plugin startup from network readiness when troubleshooting.

## Feature method reference

| Method | Meaning |
| --- | --- |
| `agreement(peer)` | Current Agreement for this feature and connection |
| `send(peer, bytes)` | SendResult; nonblocking copy of one whole message |
| `broadcast(bytes)` | bool; on the control thread, send to every Compatible client including the local client |
| `max_payload()` | `size_t`; maximum bytes after protocol and feature-name overhead |
| `fail(peer, reason)` | void; mark this feature's communication with the peer Failed |
| `on_peer(callback)` | `Result<>`; register a `void(PeerId, Agreement) noexcept` notification during Prepare |
| `on_message(callback)` | `Result<>`; register a `void(PeerId, Bytes) noexcept` delivery callback during Prepare |

Callbacks run on the control thread after activation. Message bytes are borrowed until the callback returns;
copy anything retained. Sending from a message callback is allowed. `send` can also be used from a hook
callback without waiting for the transport, but it can refuse immediately when the queue is busy.

Registration errors leave the requested callback unregistered. Captures from accepted registrations live
with the handler; objects captured by reference must live for every callback use. Feature copies share one
registration and its callbacks. For a default Feature, `agreement` and `send` return Unavailable,
`max_payload` returns zero, callback registration returns an error and `fail` has no effect.

`broadcast` skips clients that are not Compatible, continues after a refusal and returns false if any send
was refused. An empty recipient set returns true. Previously accepted sends remain queued; this is neither
an atomic operation nor a report of acknowledgments.

| `Agreement` | Meaning |
| --- | --- |
| `Unavailable` | No applicable connection, or feature inactive in this session |
| `Pending` | Peer support is being checked |
| `Compatible` | Client and host advertise the same feature ID |
| `Incompatible` | Matching support was absent, including after the negotiation deadline |
| `Failed` | This feature or its shared connection failed |

| `SendResult` | Meaning |
| --- | --- |
| `Accepted` | Complete message copied into the runtime queue |
| `Unavailable` | No applicable connection, feature or communication direction |
| `Pending` | Feature agreement is not complete |
| `Incompatible` | Peer lacks matching feature support |
| `Full` | Queue capacity or contention prevents immediate acceptance |
| `TooLarge` | Payload exceeds max_payload |
| `Failed` | Feature or connection failed |

Every refusal submits no message. Accepted transfers delivery responsibility to the runtime; it does not
confirm remote receipt or execution. A peer can disconnect after acceptance. Keep application acknowledgments
in the patch protocol only when the feature needs them. `fail` isolates a protocol failure to this feature
where possible; it does not deliberately fail unrelated features.

## Decide what a refusal means for the feature

A send result is an outcome the patch must interpret, not automatically a fatal patch error. Pending may
mean a user action should wait for agreement. Incompatible may mean an optional feature stays unavailable.
Full means the message was not queued now; a feature can retry later if that action is still relevant.
TooLarge calls for a smaller message or a protocol that splits data deliberately.

Avoid a retry loop inside a game hook or receive callback. Repeated retries can block game work or
control-thread delivery without ensuring progress. Choose a later callback or update when a retry is required, and
discard work tied to a connection that has ended. Use `fail` when continuing this protocol with the peer
would be invalid, such as receiving a message that violates its agreed format.

Use [lobby publishing](lobby.md) for named metadata attached to a hosted lobby. Use
[Lua multiplayer](lua_multiplayer.md) when game scripts should exchange ordinary Lua values through an
existing protocol. Neither requires inventing another message format for the same task.
