#pragma once

#include "diagnostic.hpp"

#include <fc/lobby.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fc::reporting {
class StatusText;
}

namespace fc::runtime {
class LobbyService;
struct PatchRecord;
} // namespace fc::runtime

// Identifies a registered item and its owning patch; the service keeps the item's values.
struct FC_LobbyItem {
    fc::runtime::LobbyService *service{};
    fc::runtime::PatchRecord *owner{};
};

// Identifies a request that remains valid until cancel, including after completion.
struct FC_LobbyRequest {
    fc::runtime::LobbyService *service{};
    fc::runtime::PatchRecord *owner{};
};

namespace fc::runtime {

// Reports native Lobby availability and this process's current and owned lobbies.
struct LobbyNativeState {
    bool available{};
    // Zero means no lobby; owned is set only after verifying local ownership.
    std::uint64_t current{}, owned{};
};

// Supplies native Lobby calls on the control thread; context and function pointers must outlive the service.
struct LobbyBackend {
    void *context{};
    LobbyNativeState (*state)(void *) noexcept {};

    // Submits one native lobby request; its listener later calls complete() with the result.
    std::expected<void, Diagnostic> (*request)(void *, std::uint64_t){};

    // Copies native text; an empty result means Missing because even empty FC values have an envelope.
    std::expected<std::string, Diagnostic> (*read)(void *, std::uint64_t, std::string_view){};

    // Writes a whole native value or removes the key with nullopt; recheck local ownership before either operation.
    std::expected<void, Diagnostic> (*write)(void *, std::uint64_t, std::string_view, std::optional<std::string_view>){};
};

// Keeps accepted values, cached reads and request tokens within one 256 KiB storage budget.
class LobbyService final {
  public:
    using Clock = std::chrono::steady_clock;

    LobbyService() = default;
    LobbyService(const LobbyService &) = delete;
    LobbyService &operator=(const LobbyService &) = delete;

    // Sets verified native key and value limits, excluding NUL, before patch Prepare; native calls need not be ready.
    void configure(std::uint32_t key_limit, std::uint32_t value_limit);

    // Keeps the native adapter for control-thread calls; missing functions leave native access unavailable.
    void attach(LobbyBackend backend);

    // Submits shared native requests, reads completed results and writes the latest accepted values on the control thread.
    void pump(Clock::time_point now);

    // Records a submitted request's first completion, including after invalidation; returns false for unmatched or duplicate results.
    // Value reads and patch callbacks wait for the control thread.
    bool complete(std::uint64_t lobby, bool success, std::optional<std::uint32_t> native_reason = std::nullopt) noexcept;

    // Finishes pending readers as Unavailable on the control thread; submitted native work remains until its old completion arrives.
    void invalidate();

    // Calls Installed patches with completed results on the control thread, attributing each call to its patch.
    // A callback may cancel its own request; its record bytes stay valid until the callback returns.
    void deliver();

    // Removes a failed patch's items and disables its request callbacks after handler cleanup on the control thread.
    void discard(PatchRecord &owner);

    // Registers an exact item name for a patch during Prepare; the native key uses the FC. prefix.
    std::expected<FC_LobbyItem *, Diagnostic> item(PatchRecord &, std::string_view name, std::uint32_t version);

    // Returns the process's hosted or joined native lobby on the control thread, or zero if absent or unavailable.
    LobbyId current() const noexcept;

    // Calls the C sink synchronously with the cached record; its bytes are valid until return. Returns false if nothing is cached.
    bool cached(LobbyId, std::string_view name, std::uint32_t version, void *capture, FC_LobbyRead);

    // Accepts a request token and defers its callback; keep the capture alive until its callback returns or cancellation completes.
    std::expected<FC_LobbyRequest *, Diagnostic> refresh(PatchRecord &, LobbyId, std::string_view name, std::uint32_t version,
                                                         void *capture, FC_LobbyRead);

    // Copies the whole latest value for later publication; refusal preserves the previously accepted value.
    std::expected<void, Diagnostic> publish(FC_LobbyItem &, Bytes value);

    // Schedules native removal and stops republishing this item's value until the next publish.
    void clear(FC_LobbyItem &);

    // Stops future callbacks and releases one accepted request token before returning.
    void cancel(FC_LobbyRequest &);

    // Appends current native write failures and requests blocked by an invalidated native operation.
    void issues(reporting::StatusText &) const;

  private:
    // Stores one native read result shared by readers that may request different versions.
    struct Observation {
        LobbyResult result = LobbyResult::Missing;
        std::uint32_t version{};
        std::vector<std::byte> value;
        Clock::time_point observed;
    };

    // Stores the cached observation shared by readers of the same lobby and item name.
    struct Cache {
        LobbyId lobby;
        std::string key;
        std::optional<Observation> observation;
        // Active requests and callbacks prevent eviction and share space reserved for the next decoded value.
        std::size_t readers{}, payload_charge{};
        std::uint64_t used{};
    };

    // Keeps the accepted value and the last successful write so unchanged values need no native write.
    struct Item final : FC_LobbyItem {
        std::string key;
        std::uint32_t version{};
        std::optional<std::string> desired, written;
        // Reserves budget for recording the next successful write when publish accepts a value; failures keep this reservation.
        std::size_t written_charge{};
        LobbyId written_lobby;
        std::optional<Clock::time_point> attempted;
        bool withdraw{};
        // Current native write failure, cleared after a successful write.
        std::optional<std::string> failure;
    };

    // Tracks callback delivery while the request's cancellation token remains valid.
    enum class RequestPhase {
        Pending,
        Ready,
        Delivering,
        Delivered
    };

    // Keeps a request token valid until cancel, including after its callback returns.
    struct Request final : FC_LobbyRequest {
        Cache *cache{};
        LobbyId lobby;
        std::uint32_t version{};
        void *capture{};
        FC_LobbyRead callback{};
        LobbyResult failure = LobbyResult::Failed;
        RequestPhase phase = RequestPhase::Pending;
    };

    // Keeps submitted native work until completion, even without readers, because the listener identifies only the lobby.
    struct Group {
        bool submitted{}, invalidated{};
        std::optional<bool> completion;
        // Native listener code; synchronous failures already carry a diagnostic from the backend call.
        std::optional<std::uint32_t> native_reason;
    };

    // Evicts the oldest cache entries without active readers before refusing additional storage.
    bool retain(std::size_t additional);

    // Releases one cache reader and clears its selection; shrinks the reservation after the last reader.
    void release_selection(Cache *&);

    // Makes a failed read ready for delivery without reusing old cached bytes or changing their observation time.
    void finish(Request &, LobbyResult failure);

    // Logs a native request or read failure for the patch that requested it, on the control thread.
    void read_failed(const Request &, std::string_view operation, const Diagnostic &);

    // Logs a failed native write for the owning patch and keeps the failure for status.
    void publication_failed(Item &, const Diagnostic &);

    LobbyBackend backend_{};
    std::uint32_t key_limit_{}, value_limit_{};
    std::size_t maximum_payload_{}, used_{};
    std::uint64_t cache_use_{};
    std::vector<std::unique_ptr<Item>> items_;
    std::vector<std::unique_ptr<Cache>> cache_;
    std::vector<std::unique_ptr<Request>> requests_;

    // Native listeners and the control thread share request groups; patch callbacks run without this lock.
    mutable std::mutex groups_mutex_;
    std::map<std::uint64_t, Group> groups_;
};

// Returns the Lobby C table for each patch; the table lives until process exit.
const FC_LobbyApi *lobby_api_host() noexcept;

} // namespace fc::runtime
