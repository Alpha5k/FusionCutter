#pragma once

#include <fc/base.hpp>
#include <fc/detail/native_types.hpp>

#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace fc {

// A process-local connection identity; zero means absent, and retaining it does not keep the connection alive.
struct PeerId {
    std::uint64_t value = 0;
    bool operator==(const PeerId &) const = default;
};

// Contains the native connection slot and Galaxy user ID associated with a peer.
struct PeerInfo {
    // Optional native connection slot, which need not be a player index; zero is valid.
    std::optional<std::uint32_t> slot;
    // Galaxy user ID; a remote host's lobby is resolved to its owner.
    std::optional<std::uint64_t> galaxy_id;
};

// Chooses whether the patch implements client behavior, host behavior, or both.
// These roles are independent of the executable's launch role and current GameMode.
enum class FeatureRole {
    Client = FC_FEATURE_CLIENT,
    Host = FC_FEATURE_HOST, // Includes dedicated servers.
    Both = FC_FEATURE_BOTH
};

// Describes this feature's compatibility on a current connection.
enum class Agreement {
    Unavailable = FC_AGREEMENT_UNAVAILABLE,   // No applicable connection, or this feature is inactive in the session.
    Pending = FC_AGREEMENT_PENDING,           // The runtime is checking the peer's feature support.
    Compatible = FC_AGREEMENT_COMPATIBLE,     // The client and host advertise the same feature ID.
    Incompatible = FC_AGREEMENT_INCOMPATIBLE, // The peer lacks matching support, including after the negotiation deadline.
    Failed = FC_AGREEMENT_FAILED              // This feature's communication or its shared connection has failed.
};

// Accepted means the runtime copied a complete message; it does not confirm remote receipt or execution.
enum class SendResult {
    Accepted = FC_SEND_ACCEPTED,
    Unavailable = FC_SEND_UNAVAILABLE,   // No applicable active connection, feature or communication direction.
    Pending = FC_SEND_PENDING,           // Feature support is still being checked.
    Incompatible = FC_SEND_INCOMPATIBLE, // The peer lacks matching feature support.
    Full = FC_SEND_FULL,                 // The outgoing queue cannot accept immediately, including lock contention.
    TooLarge = FC_SEND_TOO_LARGE,        // The payload exceeds max_payload().
    Failed = FC_SEND_FAILED              // This feature's communication or its shared connection has failed.
};

// Identifies compatible behavior and the roles implemented by one patch.
struct FeatureDefinition {
    // Change this exact ID whenever behavior or message compatibility changes.
    std::string id;
    FeatureRole role = FeatureRole::Both;
};

namespace detail {

// Returns the Multiplayer table when the host's byte_size covers its pointer.
inline const FC_MultiplayerApi *multiplayer_service(const FC_Host *host) noexcept {
    return table_available(host, offsetof(FC_Host, multiplayer), sizeof(host->multiplayer)) ? host->multiplayer : nullptr;
}

// Stores a peer callback in the patch handler's DLL.
template <class F> struct PeerCapture final : CallbackCapture {
    explicit PeerCapture(F value) : function(std::move(value)) {}

    F function;

    // Converts the C arguments and calls the patch's noexcept peer callback.
    static void FC_CDECL invoke(void *context, std::uint64_t peer, std::uint32_t agreement) noexcept {
        std::invoke(static_cast<PeerCapture *>(context)->function, PeerId{peer}, static_cast<Agreement>(agreement));
    }
};

// Stores a message callback in the patch handler's DLL.
template <class F> struct MessageCapture final : CallbackCapture {
    explicit MessageCapture(F value) : function(std::move(value)) {}

    F function;

    // Passes one complete payload to the callback; the patch must copy bytes it keeps after returning.
    static void FC_CDECL invoke(void *context, std::uint64_t peer, FC_Bytes bytes) noexcept {
        std::invoke(static_cast<MessageCapture *>(context)->function, PeerId{peer},
                    Bytes{reinterpret_cast<const std::byte *>(bytes.data), bytes.size});
    }
};

} // namespace detail

class Multiplayer;

// Provides agreement and messaging for one registered feature; the patch's handler owns its callback storage.
class Feature {
  public:
    Feature() = default;

    // Returns current compatibility; an ended peer or nonparticipating registration is Unavailable.
    Agreement agreement(PeerId peer) const noexcept {
        const auto *api = detail::multiplayer_service(access_.host);
        return record_ && detail::table_available(api, offsetof(FC_MultiplayerApi, agreement), sizeof(api->agreement)) && api->agreement
                   ? static_cast<Agreement>(api->agreement(record_, peer.value))
                   : Agreement::Unavailable;
    }

    // Queues a complete copy without waiting, including sends to the local client or from a receive callback.
    // Accepted transfers responsibility to the runtime; other results submit nothing. Remote delivery is not confirmed.
    SendResult send(PeerId peer, Bytes bytes) noexcept {
        if (bytes.size() > std::numeric_limits<std::uint32_t>::max()) {
            return SendResult::TooLarge;
        }

        const auto *api = detail::multiplayer_service(access_.host);
        if (!record_ || !detail::table_available(api, offsetof(FC_MultiplayerApi, send), sizeof(api->send)) || !api->send) {
            return SendResult::Unavailable;
        }

        return static_cast<SendResult>(api->send(
            record_, peer.value, {reinterpret_cast<const std::uint8_t *>(bytes.data()), static_cast<std::uint32_t>(bytes.size())}));
    }

    // On the control thread, sends to every Compatible client, including the local client; an empty set returns true.
    // Continues after refusals and returns false if any send was refused; earlier accepted sends remain queued.
    bool broadcast(Bytes bytes);

    // Returns the maximum payload size in bytes, allowing space for the frame header and feature ID.
    std::size_t max_payload() const noexcept {
        const auto *api = detail::multiplayer_service(access_.host);
        return record_ && detail::table_available(api, offsetof(FC_MultiplayerApi, max_payload), sizeof(api->max_payload)) &&
                       api->max_payload
                   ? api->max_payload(record_)
                   : 0;
    }

    // Marks this feature's communication with the peer Failed without failing other features on the connection.
    void fail(PeerId peer, std::string_view reason) noexcept {
        const auto *api = detail::multiplayer_service(access_.host);
        if (record_ && detail::table_available(api, offsetof(FC_MultiplayerApi, fail), sizeof(api->fail)) && api->fail) {
            // Truncate oversized reasons to the C ABI's 32-bit count limit so fail() stays noexcept.
            const auto size = reason.size() > UINT32_MAX ? UINT32_MAX : static_cast<std::uint32_t>(reason.size());
            api->fail(record_, peer.value, {reason.data(), size});
        }
    }

    // Registers F(PeerId, Agreement) during prepare(); F must be noexcept.
    // The runtime calls it after activation on this patch's serialized control thread.
    template <class F> Result<> on_peer(F &&function) {
        using Capture = detail::PeerCapture<std::decay_t<F>>;
        static_assert(std::is_nothrow_invocable_v<std::decay_t<F> &, PeerId, Agreement>,
                      "Feature peer callbacks must accept PeerId and Agreement without throwing (noexcept)");
        const auto *api = detail::multiplayer_service(access_.host);
        if (!record_ || !captures_ || !detail::table_available(api, offsetof(FC_MultiplayerApi, on_peer), sizeof(api->on_peer)) ||
            !api->on_peer) {
            return std::unexpected(Error{"Feature peer callbacks are unavailable"});
        }

        auto capture = std::make_unique<Capture>(std::forward<F>(function));
        auto *context = capture.get();
        // The handler owns the callable before registration gives its address to the runtime.
        captures_->push_back(std::move(capture));
        detail::NativeError error;
        if (api->on_peer(record_, context, &Capture::invoke, error.sink()) != 1) {
            // Registration failed, so the runtime will not call this capture and the handler can release it.
            captures_->pop_back();
            return std::unexpected(error.value.message.empty() ? Error{"Could not register feature peer callback"} : error.value);
        }

        return {};
    }

    // Registers F(PeerId, Bytes) during prepare(); F must be noexcept.
    // The runtime calls it on this patch's serialized control thread; copy bytes kept after the callback returns.
    template <class F> Result<> on_message(F &&function) {
        using Capture = detail::MessageCapture<std::decay_t<F>>;
        static_assert(std::is_nothrow_invocable_v<std::decay_t<F> &, PeerId, Bytes>,
                      "Feature message callbacks must accept PeerId and Bytes without throwing (noexcept)");
        const auto *api = detail::multiplayer_service(access_.host);
        if (!record_ || !captures_ || !detail::table_available(api, offsetof(FC_MultiplayerApi, on_message), sizeof(api->on_message)) ||
            !api->on_message) {
            return std::unexpected(Error{"Feature message callbacks are unavailable"});
        }

        auto capture = std::make_unique<Capture>(std::forward<F>(function));
        auto *context = capture.get();
        // The handler owns the callable before registration gives its address to the runtime.
        captures_->push_back(std::move(capture));
        detail::NativeError error;
        if (api->on_message(record_, context, &Capture::invoke, error.sink()) != 1) {
            // Registration failed, so the runtime will not call this capture and the handler can release it.
            captures_->pop_back();
            return std::unexpected(error.value.message.empty() ? Error{"Could not register feature message callback"} : error.value);
        }

        return {};
    }

  private:
    friend class Multiplayer;

    Feature(detail::StageAccess access, FC_Feature *record, detail::CallbackCaptures *captures) noexcept
        : access_(access), record_(record), captures_(captures) {}

    detail::StageAccess access_{};
    FC_Feature *record_{};
    detail::CallbackCaptures *captures_{};
};

// Lets a patch inspect peers and register features; this handle can be kept after prepare() returns.
class Multiplayer {
  public:
    Multiplayer() = default;

    explicit Multiplayer(detail::StageAccess access, detail::CallbackCaptures *captures = nullptr) noexcept
        : access_(access), captures_(captures) {}

    // Returns the host endpoint, including this process's host when hosting; zero means absent.
    PeerId host() const noexcept {
        const auto *api = detail::multiplayer_service(access_.host);
        return {detail::table_available(api, offsetof(FC_MultiplayerApi, host), sizeof(api->host)) && api->host ? api->host(access_.owner)
                                                                                                                : 0};
    }

    // Returns this process's local client endpoint; a dedicated host has none, and a playing host uses a distinct ID.
    PeerId local() const noexcept {
        const auto *api = detail::multiplayer_service(access_.host);
        return {detail::table_available(api, offsetof(FC_MultiplayerApi, local), sizeof(api->local)) && api->local
                    ? api->local(access_.owner)
                    : 0};
    }

    // On the control thread, copies the host's connected clients, including its local client.
    // Returns an empty list when this process is a remote client.
    std::vector<PeerId> clients() const {
        const auto *api = detail::multiplayer_service(access_.host);
        if (!detail::table_available(api, offsetof(FC_MultiplayerApi, clients), sizeof(api->clients)) || !api->clients) {
            return {};
        }

        struct Copy {
            std::vector<PeerId> peers;
            std::exception_ptr error;
        } copied;

        // This DLL owns the copied list. Catch allocation errors in the C callback and rethrow after the runtime returns.
        api->clients(access_.owner, {&copied, [](void *context, const std::uint64_t *peers, std::uint32_t count) noexcept -> std::uint32_t {
                                         auto &copy = *static_cast<Copy *>(context);
                                         try {
                                             for (std::uint32_t i = 0; i < count; ++i) {
                                                 copy.peers.push_back({peers[i]});
                                             }
                                             return 1;
                                         } catch (...) {
                                             copy.error = std::current_exception();
                                             return 0;
                                         }
                                     }});

        if (copied.error) {
            std::rethrow_exception(copied.error);
        }

        return std::move(copied.peers);
    }

    // Copies a current peer's native slot and Galaxy user ID; nullopt means unavailable or expired.
    // Keeping the copy does not keep the connection alive.
    std::optional<PeerInfo> info(PeerId peer) const noexcept {
        const auto *api = detail::multiplayer_service(access_.host);
        if (!detail::table_available(api, offsetof(FC_MultiplayerApi, info), sizeof(api->info)) || !api->info) {
            return std::nullopt;
        }

        FC_PeerInfo value{};
        if (api->info(access_.owner, peer.value, &value) != 1) {
            return std::nullopt;
        }

        return PeerInfo{value.has_slot ? std::optional{value.slot} : std::nullopt,
                        value.has_galaxy_id ? std::optional{value.galaxy_id} : std::nullopt};
    }

    // Registers the exact behavior ID and roles during prepare().
    // Game Start freezes support from successfully activated patches; later activations wait for the next session.
    Result<Feature> feature(FeatureDefinition definition) {
        const auto *api = detail::multiplayer_service(access_.host);
        if (!detail::table_available(api, offsetof(FC_MultiplayerApi, register_feature), sizeof(api->register_feature)) ||
            !api->register_feature) {
            return std::unexpected(Error{"Multiplayer feature registration is unavailable"});
        }

        const FC_FeatureDefinition input{detail::text(definition.id), static_cast<std::uint32_t>(definition.role)};
        FC_Feature *record{};
        detail::NativeError error;
        if (api->register_feature(access_.owner, &input, &record, error.sink()) != 1 || !record) {
            return std::unexpected(error.value.message.empty() ? Error{"Could not register multiplayer feature"} : error.value);
        }

        return Feature{access_, record, captures_};
    }

  private:
    detail::StageAccess access_{};
    detail::CallbackCaptures *captures_{};
};

inline bool Feature::broadcast(Bytes bytes) {
    bool accepted = true;

    for (const auto peer : Multiplayer{access_}.clients()) {
        if (agreement(peer) == Agreement::Compatible && send(peer, bytes) != SendResult::Accepted) {
            accepted = false;
        }
    }

    return accepted;
}

} // namespace fc
