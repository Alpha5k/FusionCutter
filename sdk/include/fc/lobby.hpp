#pragma once

#include <fc/base.hpp>
#include <fc/detail/native_types.hpp>
#include <fc/reporting.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace fc {

// Identifies a native lobby; zero means absent.
struct LobbyId {
    std::uint64_t value = 0;
    bool operator==(const LobbyId &) const = default;
};

// Describes the result of reading one named lobby item.
enum class LobbyResult {
    Value = FC_LOBBY_VALUE,             // Valid item in the requested format.
    Missing = FC_LOBBY_MISSING,         // Successful native read found no item.
    Malformed = FC_LOBBY_MALFORMED,     // Stored FC record could not be decoded.
    Unsupported = FC_LOBBY_UNSUPPORTED, // Envelope or requested payload format is unsupported.
    Unavailable = FC_LOBBY_UNAVAILABLE, // Required native access is unavailable.
    Failed = FC_LOBBY_FAILED            // Native request or read failed.
};

// Contains one lobby read result and its copied payload.
struct LobbyRecord {
    LobbyId lobby;
    LobbyResult result;
    // Observed payload format when readable; failed requests supply no valid version.
    std::uint32_t version;
    std::vector<std::byte> value;
    // Time of the successful native read; failed requests supply a default time.
    std::chrono::steady_clock::time_point observed;
};

namespace detail {

// Returns the Lobby table when the host's byte_size covers its pointer.
inline const FC_LobbyApi *lobby_service(const FC_Host *host) noexcept {
    return table_available(host, offsetof(FC_Host, lobby), sizeof(host->lobby)) ? host->lobby : nullptr;
}

// Copies the runtime's record and payload into this DLL and converts nanoseconds to a steady-clock time.
inline LobbyRecord lobby_record(const FC_LobbyRecord &record) {
    LobbyRecord copy{{record.lobby},
                     static_cast<LobbyResult>(record.result),
                     record.version,
                     {},
                     std::chrono::steady_clock::time_point{
                         std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::nanoseconds{record.observed_ns})}};
    if (record.value.size) {
        const auto *bytes = reinterpret_cast<const std::byte *>(record.value.data);
        copy.value.assign(bytes, bytes + record.value.size);
    }

    return copy;
}

// Keeps a synchronous copy failure inside the C boundary until cached() can rethrow it.
struct LobbyCopy {
    std::optional<LobbyRecord> record;
    std::exception_ptr failure;

    // Copies the record and saves any exception for cached() to rethrow after the C call returns.
    static void FC_CDECL invoke(void *context, const FC_LobbyRecord *record) noexcept {
        auto &copy = *static_cast<LobbyCopy *>(context);
        try {
            copy.record = lobby_record(*record);
        } catch (...) {
            copy.failure = std::current_exception();
        }
    }
};

// Keeps the refresh callback in the patch's DLL until completion or Request destruction.
template <class F> struct LobbyCapture final : CallbackCapture {
    explicit LobbyCapture(F value) : function(std::move(value)) {}

    F function;

    // Move the callable out of Request's storage before invoking it, so the callback can destroy its own Request.
    static void FC_CDECL invoke(void *context, const FC_LobbyRecord *record) noexcept {
        auto &capture = *static_cast<LobbyCapture *>(context);
        std::optional<F> function;
        std::optional<LobbyRecord> copy;
        try {
            function.emplace(std::move(capture.function));
            copy = lobby_record(*record);
        } catch (...) {
            fatal(Error{"Could not prepare a lobby read callback"});
        }

        std::invoke(*function, std::move(*copy));
    }
};

} // namespace detail

// Owns one refresh callback and its cancellation; destroy or replace it on the control thread to cancel delivery.
class Request {
  public:
    Request() = default;
    Request(const Request &) = delete;
    Request &operator=(const Request &) = delete;

    // Transfers the request and callback storage; the moved-from Request no longer cancels it.
    Request(Request &&other) noexcept
        : access_(other.access_), record_(std::exchange(other.record_, nullptr)), capture_(std::move(other.capture_)) {}

    // Cancels this request before transferring the replacement's callback storage.
    Request &operator=(Request &&other) noexcept {
        if (this != &other) {
            release();
            access_ = other.access_;
            record_ = std::exchange(other.record_, nullptr);
            capture_ = std::move(other.capture_);
        }

        return *this;
    }

    // Cancels the runtime request before destroying the callback storage in this DLL.
    ~Request() {
        release();
    }

  private:
    friend class Lobby;

    Request(detail::StageAccess access, FC_LobbyRequest *record, std::unique_ptr<detail::CallbackCapture> capture) noexcept
        : access_(access), record_(record), capture_(std::move(capture)) {}

    // The runtime keeps accepted request records valid until cancel(), even after completion.
    // Cancel before destroying callback storage so the runtime cannot use a freed capture.
    void release() noexcept {
        if (record_) {
            detail::lobby_service(access_.host)->cancel(std::exchange(record_, nullptr));
        }

        capture_.reset();
    }

    detail::StageAccess access_{};
    FC_LobbyRequest *record_{};
    std::unique_ptr<detail::CallbackCapture> capture_;
};

// Represents one name and payload format registered by this patch during prepare().
// Call publish() and clear() on the control thread; publication can be accepted before a lobby is active.
class LobbyItem {
  public:
    LobbyItem() = default;

    LobbyItem(detail::StageAccess access, FC_LobbyItem *record) noexcept : access_(access), record_(record) {}

    // Copies the latest whole value for publication in a lobby owned by this process; empty bytes are valid.
    // Acceptance means the runtime stored the value; it does not confirm a native write.
    Result<> publish(Bytes value) {
        const auto *api = detail::lobby_service(access_.host);
        if (!record_ || !detail::table_available(api, offsetof(FC_LobbyApi, publish), sizeof(api->publish)) || !api->publish) {
            return std::unexpected(Error{"Lobby publication is unavailable"});
        }
        if (value.size() > UINT32_MAX) {
            return std::unexpected(Error{"Lobby value exceeds the C ABI's 32-bit count limit"});
        }

        detail::NativeError error;
        if (api->publish(record_, {reinterpret_cast<const std::uint8_t *>(value.data()), detail::count(value.size())}, error.sink()) != 1) {
            return std::unexpected(error.value.message.empty() ? Error{"Could not publish lobby item"} : error.value);
        }

        return {};
    }

    // Withdraws this item and prevents republication until another publish; an empty handle has no effect.
    void clear() {
        const auto *api = detail::lobby_service(access_.host);
        if (record_ && detail::table_available(api, offsetof(FC_LobbyApi, clear), sizeof(api->clear)) && api->clear) {
            api->clear(record_);
        }
    }

  private:
    detail::StageAccess access_{};
    FC_LobbyItem *record_{};
};

// Lets a patch register items, publish values and read lobbies; keep this handle as needed after prepare().
// Call every operation on the control thread.
class Lobby {
  public:
    Lobby() = default;

    explicit Lobby(detail::StageAccess access) noexcept : access_(access) {}

    // Registers this patch as the publisher of one name and payload format during prepare().
    // Reading a name does not reserve it for publication.
    Result<LobbyItem> item(std::string_view name, std::uint32_t version) {
        const auto *api = detail::lobby_service(access_.host);
        if (!detail::table_available(api, offsetof(FC_LobbyApi, register_item), sizeof(api->register_item)) || !api->register_item) {
            return std::unexpected(Error{"Lobby item registration is unavailable"});
        }

        detail::NativeError error;
        FC_LobbyItem *record{};
        if (api->register_item(access_.owner, detail::text(name), version, &record, error.sink()) != 1) {
            return std::unexpected(error.value.message.empty() ? Error{"Could not register lobby item"} : error.value);
        }

        return LobbyItem{access_, record};
    }

    // Returns the hosted or joined lobby, or zero when absent or unavailable.
    LobbyId current() const noexcept {
        const auto *api = detail::lobby_service(access_.host);
        return detail::table_available(api, offsetof(FC_LobbyApi, current), sizeof(api->current)) && api->current
                   ? LobbyId{api->current(access_.owner)}
                   : LobbyId{};
    }

    // Copies the cached record immediately; nullopt means no cached record, while Missing means a read found no item.
    std::optional<LobbyRecord> cached(LobbyId lobby, std::string_view name, std::uint32_t version) const {
        const auto *api = detail::lobby_service(access_.host);
        if (!detail::table_available(api, offsetof(FC_LobbyApi, cached), sizeof(api->cached)) || !api->cached) {
            return std::nullopt;
        }

        detail::LobbyCopy copy;
        const auto found = api->cached(access_.owner, lobby.value, detail::text(name), version, &copy, &detail::LobbyCopy::invoke);
        if (copy.failure) {
            std::rethrow_exception(copy.failure);
        }

        return found == 1 ? std::move(copy.record) : std::nullopt;
    }

    // Reads asynchronously and calls F(LobbyRecord) once with a copied result on the control thread; F must be noexcept.
    // Keep the returned Request until completion; destroying it cancels delivery. The callback never runs inside refresh().
    template <class F> Result<Request> refresh(LobbyId lobby, std::string_view name, std::uint32_t version, F function) {
        using Capture = detail::LobbyCapture<F>;
        static_assert(std::is_nothrow_invocable_v<F &, LobbyRecord>,
                      "Lobby refresh callbacks must accept a LobbyRecord without throwing (noexcept)");
        const auto *api = detail::lobby_service(access_.host);
        if (!detail::table_available(api, offsetof(FC_LobbyApi, cancel), sizeof(api->cancel)) || !api->refresh || !api->cancel) {
            return std::unexpected(Error{"Lobby refresh is unavailable"});
        }

        auto capture = std::make_unique<Capture>(std::move(function));
        detail::NativeError error;
        FC_LobbyRequest *record{};
        if (api->refresh(access_.owner, lobby.value, detail::text(name), version, capture.get(), &Capture::invoke, &record, error.sink()) !=
            1) {
            return std::unexpected(error.value.message.empty() ? Error{"Could not refresh lobby item"} : error.value);
        }

        return Request{access_, record, std::move(capture)};
    }

  private:
    detail::StageAccess access_{};
};

} // namespace fc
