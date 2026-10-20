#include "lobby.hpp"

#include "core_runtime.hpp"
#include "fatal_boundary.hpp"
#include "patches/patch_api.hpp"
#include "patches/patch_record.hpp"
#include "reporting/status.hpp"

#include <algorithm>
#include <charconv>
#include <format>
#include <utility>

namespace fc::runtime {
namespace {

constexpr std::size_t kStorageBytes = 256U * 1024U;
constexpr std::string_view kKeyPrefix = "FC.";
constexpr std::string_view kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// Forms the native key with the FC. prefix; rejects names that contain NUL or exceed the native key limit.
std::optional<std::string> key_for(std::string_view name, std::uint32_t limit) {
    if (name.empty() || name.find('\0') != std::string_view::npos || name.size() > limit || kKeyPrefix.size() > limit - name.size()) {
        return std::nullopt;
    }
    return std::string{kKeyPrefix} + std::string{name};
}

// Returns the public name as a view into its owned, qualified native key.
std::string_view public_name(std::string_view key) noexcept {
    return key.substr(kKeyPrefix.size());
}

// Encodes a decimal version and base64 payload in one FC1 value after the caller checks the native text limit.
std::string encode(std::uint32_t version, Bytes bytes) {
    std::string text = std::format("FC1:{}:", version);
    text.reserve(text.size() + ((bytes.size() + 2) / 3) * 4);

    for (std::size_t i = 0; i < bytes.size(); i += 3) {
        const auto first = std::to_integer<std::uint32_t>(bytes[i]);
        const auto second = i + 1 < bytes.size() ? std::to_integer<std::uint32_t>(bytes[i + 1]) : 0;
        const auto third = i + 2 < bytes.size() ? std::to_integer<std::uint32_t>(bytes[i + 2]) : 0;
        text.push_back(kAlphabet[first >> 2]);
        text.push_back(kAlphabet[((first & 3) << 4) | (second >> 4)]);
        text.push_back(i + 1 < bytes.size() ? kAlphabet[((second & 15) << 2) | (third >> 6)] : '=');
        text.push_back(i + 2 < bytes.size() ? kAlphabet[third & 63] : '=');
    }
    return text;
}

// Stores decoded FC bytes or the result returned when native text contains no usable value.
struct Decoded {
    LobbyResult result = LobbyResult::Malformed;
    std::uint32_t version{};
    std::vector<std::byte> value;
};

// Checks the FC1 envelope, decimal version and canonical base64, then returns decoded bytes owned by the result.
Decoded decode(std::string_view text, std::size_t value_limit) {
    if (text.empty()) {
        return {LobbyResult::Missing};
    }
    if (text.size() > value_limit) {
        return {};
    }
    if (!text.starts_with("FC1:")) {
        const auto colon = text.find(':');
        const bool another_envelope = text.starts_with("FC") && colon != std::string_view::npos && colon > 2 &&
                                      std::ranges::all_of(text.substr(2, colon - 2), [](char value) {
                                          return value >= '0' && value <= '9';
                                      });
        return {another_envelope ? LobbyResult::Unsupported : LobbyResult::Malformed};
    }

    const auto colon = text.find(':', 4);
    if (colon == std::string_view::npos || colon == 4) {
        return {};
    }

    Decoded result;
    const auto version = text.substr(4, colon - 4);
    const auto parsed = std::from_chars(version.data(), version.data() + version.size(), result.version);
    if (parsed.ec != std::errc{} || parsed.ptr != version.data() + version.size()) {
        return {};
    }

    const auto payload = text.substr(colon + 1);
    if (payload.size() % 4 != 0) {
        return {};
    }
    result.value.reserve((payload.size() / 4) * 3);
    for (std::size_t i = 0; i < payload.size(); i += 4) {
        // Padding belongs only in the final quartet; its unused bits must be zero.
        std::uint32_t digits[4]{};
        std::size_t padding{};
        for (std::size_t j = 0; j != 4; ++j) {
            if (payload[i + j] == '=') {
                if (j < 2 || i + 4 != payload.size()) {
                    return {};
                }
                ++padding;
            } else {
                const auto index = kAlphabet.find(payload[i + j]);
                if (padding || index == std::string_view::npos) {
                    return {};
                }
                digits[j] = static_cast<std::uint32_t>(index);
            }
        }

        if ((padding == 2 && (digits[1] & 15)) || (padding == 1 && (digits[2] & 3))) {
            return {};
        }
        result.value.push_back(static_cast<std::byte>((digits[0] << 2) | (digits[1] >> 4)));
        if (padding < 2) {
            result.value.push_back(static_cast<std::byte>((digits[1] << 4) | (digits[2] >> 2)));
        }
        if (padding == 0) {
            result.value.push_back(static_cast<std::byte>((digits[2] << 6) | digits[3]));
        }
    }

    result.result = LobbyResult::Value;
    return result;
}

// Creates a C record that refers to cached bytes; a version mismatch reports Unsupported with no payload.
FC_LobbyRecord record_for(LobbyId lobby, LobbyResult result, std::uint32_t observed_version, Bytes value,
                          LobbyService::Clock::time_point observed, std::uint32_t requested_version) {
    if (result == LobbyResult::Value && observed_version != requested_version) {
        result = LobbyResult::Unsupported;
        value = {};
    }

    return {lobby.value,
            static_cast<std::uint32_t>(result),
            observed_version,
            {reinterpret_cast<const std::uint8_t *>(value.data()), static_cast<std::uint32_t>(value.size())},
            std::chrono::duration_cast<std::chrono::nanoseconds>(observed.time_since_epoch()).count()};
}

} // namespace

void LobbyService::configure(std::uint32_t key_limit, std::uint32_t value_limit) {
    // Count one temporary native text buffer up front; pending reads separately reserve decoded payload space.
    used_ -= value_limit_;
    key_limit_ = key_limit;
    value_limit_ = value_limit;

    // The shortest FC1 header is six characters; four base64 characters hold up to three bytes.
    maximum_payload_ = value_limit >= 6 ? ((value_limit - 6) / 4) * 3 : 0;
    used_ += value_limit_;
}

void LobbyService::attach(LobbyBackend backend) {
    backend_ = backend;
}

bool LobbyService::retain(std::size_t additional) {
    while (additional > kStorageBytes - used_) {
        auto oldest = cache_.end();
        for (auto entry = cache_.begin(); entry != cache_.end(); ++entry) {
            if ((*entry)->readers == 0 && (oldest == cache_.end() || (*entry)->used < (*oldest)->used)) {
                oldest = entry;
            }
        }
        if (oldest == cache_.end()) {
            return false;
        }
        const auto &entry = **oldest;
        used_ -= sizeof(Cache) + sizeof(std::unique_ptr<Cache>) + entry.key.size() + entry.payload_charge;
        cache_.erase(oldest);
    }
    return true;
}

std::expected<FC_LobbyItem *, Diagnostic> LobbyService::item(PatchRecord &owner, std::string_view name, std::uint32_t version) {
    if (owner.state.load(std::memory_order_acquire) != PatchState::Preparing) {
        return std::unexpected(Diagnostic{qualified_patch_id(owner), "Register Lobby items during prepare()"});
    }
    if (!key_limit_) {
        return std::unexpected(
            Diagnostic{qualified_patch_id(owner), "Lobby item registration is unavailable: the native key limit is unknown"});
    }
    const auto key = key_for(name, key_limit_);
    if (!key) {
        return std::unexpected(Diagnostic{
            qualified_patch_id(owner), "Lobby item name is empty, contains NUL, or exceeds the native key limit including the FC. prefix"});
    }
    if (std::ranges::any_of(items_, [&](const auto &entry) {
            return public_name(entry->key) == name;
        })) {
        return std::unexpected(Diagnostic{qualified_patch_id(owner), std::format("A patch already owns Lobby item '{}'", name)});
    }
    const auto charge = sizeof(Item) + sizeof(std::unique_ptr<Item>) + key->size();
    if (!retain(charge)) {
        return std::unexpected(Diagnostic{qualified_patch_id(owner), "Lobby item storage exceeds the shared 256 KiB budget"});
    }

    auto item = std::make_unique<Item>();
    item->service = this;
    item->owner = &owner;
    item->key = *key;
    item->version = version;
    auto *out = item.get();
    items_.push_back(std::move(item));
    used_ += charge;
    return out;
}

LobbyId LobbyService::current() const noexcept {
    const auto state = backend_.state ? backend_.state(backend_.context) : LobbyNativeState{};
    return {state.available ? state.current : 0};
}

bool LobbyService::cached(LobbyId lobby, std::string_view name, std::uint32_t version, void *capture, FC_LobbyRead callback) {
    const auto found = std::ranges::find_if(cache_, [&](const auto &entry) {
        return entry->lobby == lobby && public_name(entry->key) == name;
    });
    if (found == cache_.end() || !(*found)->observation || !callback) {
        return false;
    }
    auto &entry = **found;
    entry.used = ++cache_use_;

    // Keep these bytes valid if the synchronous callback causes other cache entries to be evicted.
    ++entry.readers;
    const auto &observation = *entry.observation;
    const auto record = record_for(lobby, observation.result, observation.version, observation.value, observation.observed, version);
    fc::invoke_patch_callback([&] {
        callback(capture, &record);
    });

    auto *selection = &entry;
    release_selection(selection);
    return true;
}

std::expected<FC_LobbyRequest *, Diagnostic> LobbyService::refresh(PatchRecord &owner, LobbyId lobby, std::string_view name,
                                                                   std::uint32_t version, void *capture, FC_LobbyRead callback) {
    if (!key_limit_) {
        return std::unexpected(Diagnostic{qualified_patch_id(owner), "Lobby refresh is unavailable: the native key limit is unknown"});
    }
    const auto key = key_for(name, key_limit_);
    if (!lobby.value || !key || !callback) {
        return std::unexpected(Diagnostic{qualified_patch_id(owner), "Lobby refresh requires a lobby, valid item name and callback"});
    }

    const auto native = backend_.state ? backend_.state(backend_.context) : LobbyNativeState{};
    const bool available = native.available && backend_.request && backend_.read && key_limit_ && value_limit_;
    const auto found = std::ranges::find_if(cache_, [&](const auto &entry) {
        return entry->lobby == lobby && public_name(entry->key) == name;
    });
    Cache *selection = available && found != cache_.end() ? found->get() : nullptr;
    const auto previous_readers = selection ? selection->readers : 0;
    if (selection) {
        // Keep this cache entry from being evicted while making room for the new request.
        ++selection->readers;
    }

    const auto cache_charge = available ? (selection ? maximum_payload_ - selection->payload_charge
                                                     : sizeof(Cache) + sizeof(std::unique_ptr<Cache>) + key->size() + maximum_payload_)
                                        : 0;
    bool new_group{};
    {
        std::lock_guard lock(groups_mutex_);
        new_group = available && !groups_.contains(lobby.value);
    }
    // Count the map node's links with its group record in the shared storage budget.
    const auto group_charge = new_group ? sizeof(std::pair<const std::uint64_t, Group>) + 4 * sizeof(void *) : 0;
    const auto request_charge = sizeof(Request) + sizeof(std::unique_ptr<Request>);
    if (!retain(cache_charge + group_charge + request_charge)) {
        if (selection) {
            selection->readers = previous_readers;
        }
        return std::unexpected(Diagnostic{qualified_patch_id(owner), "Lobby refresh storage exceeds the shared 256 KiB budget"});
    }

    try {
        if (available && !selection) {
            auto entry = std::make_unique<Cache>();
            entry->lobby = lobby;
            entry->key = *key;
            entry->used = ++cache_use_;
            entry->payload_charge = maximum_payload_;
            auto *created = entry.get();
            cache_.push_back(std::move(entry));
            selection = created;
            used_ += cache_charge;
            ++selection->readers;
        } else if (selection) {
            selection->payload_charge = maximum_payload_;
            used_ += cache_charge;
        }

        if (new_group) {
            std::lock_guard lock(groups_mutex_);
            groups_.emplace(lobby.value, Group{});
            used_ += group_charge;
        }

        auto request = std::make_unique<Request>();
        request->service = this;
        request->owner = &owner;
        request->lobby = lobby;
        request->version = version;
        request->cache = selection;
        request->capture = capture;
        request->callback = callback;
        request->phase = available ? RequestPhase::Pending : RequestPhase::Ready;
        request->failure = LobbyResult::Unavailable;
        auto *out = request.get();
        requests_.push_back(std::move(request));
        used_ += request_charge;
        return out;
    } catch (...) {
        release_selection(selection);
        throw;
    }
}

void LobbyService::release_selection(Cache *&selection) {
    if (!selection) {
        return;
    }

    auto &entry = *std::exchange(selection, nullptr);
    --entry.readers;
    if (entry.readers == 0) {
        // Drop the spare payload reservation after the last reader; cached bytes remain charged.
        const auto payload = entry.observation ? entry.observation->value.capacity() : 0;
        used_ -= entry.payload_charge - payload;
        entry.payload_charge = payload;
    }
}

void LobbyService::finish(Request &request, LobbyResult failure) {
    release_selection(request.cache);
    request.failure = failure;
    request.phase = RequestPhase::Ready;
}

std::expected<void, Diagnostic> LobbyService::publish(FC_LobbyItem &registration, Bytes value) {
    auto &item = static_cast<Item &>(registration);
    const auto prefix = std::format("FC1:{}:", item.version);
    if (!value_limit_ || value.size() > maximum_payload_ || prefix.size() + ((value.size() + 2) / 3) * 4 > value_limit_) {
        return std::unexpected(Diagnostic{
            qualified_patch_id(*item.owner),
            std::format("Cannot publish Lobby key '{}': the value does not fit the native text limit with FC1 encoding", item.key)});
    }

    auto encoded = encode(item.version, value);

    // Charge the new desired value and space for recording a successful write before replacing the accepted value.
    const auto previous = item.desired ? item.desired->size() : 0;
    const auto written_charge = std::max(item.written ? item.written->size() : 0, encoded.size());
    const auto previous_total = previous + item.written_charge;
    const auto next_total = encoded.size() + written_charge;
    if (next_total > previous_total && !retain(next_total - previous_total)) {
        return std::unexpected(Diagnostic{qualified_patch_id(*item.owner), "Lobby publication exceeds the shared 256 KiB budget"});
    }

    used_ = used_ - previous_total + next_total;
    item.written_charge = written_charge;
    item.desired = std::move(encoded);
    item.withdraw = false;
    return {};
}

void LobbyService::clear(FC_LobbyItem &registration) {
    auto &item = static_cast<Item &>(registration);
    if (item.desired) {
        used_ -= item.desired->size();
        item.desired.reset();
    }

    item.withdraw = true;
    const auto written_charge = item.written ? item.written->size() : 0;
    used_ -= item.written_charge - written_charge;
    item.written_charge = written_charge;
}

void LobbyService::cancel(FC_LobbyRequest &registration) {
    auto &request = static_cast<Request &>(registration);
    release_selection(request.cache);
    const auto found = std::ranges::find_if(requests_, [&](const auto &entry) {
        return entry.get() == &request;
    });
    used_ -= sizeof(Request) + sizeof(std::unique_ptr<Request>);
    requests_.erase(found);
}

bool LobbyService::complete(std::uint64_t lobby, bool success, std::optional<std::uint32_t> native_reason) noexcept {
    std::lock_guard lock(groups_mutex_);
    const auto found = groups_.find(lobby);
    if (found != groups_.end() && found->second.submitted && !found->second.completion) {
        found->second.completion = success;
        found->second.native_reason = native_reason;
        return true;
    }
    return false;
}

void LobbyService::invalidate() {
    for (const auto &request : requests_) {
        if (request->phase == RequestPhase::Pending) {
            finish(*request, LobbyResult::Unavailable);
        }
    }

    std::lock_guard lock(groups_mutex_);
    for (auto &[lobby, group] : groups_) {
        if (group.submitted) {
            group.invalidated = true;
        }
    }
}

void LobbyService::publication_failed(Item &item, const Diagnostic &error) {
    const auto failure = diagnostic_text(error);
    const bool changed = !item.failure || *item.failure != failure;
    item.failure = failure;
    if (changed && item.owner->runtime) {
        const auto text = std::format("Lobby key '{}': {}", item.key, *item.failure);
        item.owner->runtime->reporting().write(item.owner, FC_LOG_WARNING, fc::detail::text(text));
    }
}

void LobbyService::read_failed(const Request &request, std::string_view operation, const Diagnostic &error) {
    if (request.owner->runtime) {
        const auto text =
            std::format("Lobby {} key '{}': {} failed: {}", request.lobby.value, request.cache->key, operation, diagnostic_text(error));
        request.owner->runtime->reporting().write(request.owner, FC_LOG_WARNING, fc::detail::text(text));
    }
}

void LobbyService::pump(Clock::time_point now) {
    const auto native = backend_.state ? backend_.state(backend_.context) : LobbyNativeState{};
    const bool available = native.available && backend_.request && backend_.read && backend_.write;
    if (!available) {
        invalidate();
    }

    for (auto group = groups_.begin(); group != groups_.end();) {
        const auto lobby = group->first;
        bool submitted{}, invalidated{};
        std::optional<bool> completion;
        std::optional<std::uint32_t> native_reason;

        // Copy the request group's state under the lock; native calls below run without it.
        {
            std::lock_guard lock(groups_mutex_);
            submitted = group->second.submitted;
            invalidated = group->second.invalidated;
            completion = group->second.completion;
            native_reason = group->second.native_reason;
        }

        const auto awaiting = [&] {
            return std::ranges::any_of(requests_, [&](const auto &request) {
                return request->lobby.value == lobby && request->phase == RequestPhase::Pending;
            });
        };
        if (completion && invalidated) {
            // The listener identifies only the lobby; consume an invalidated request's completion before submitting new work.
            std::lock_guard lock(groups_mutex_);
            group->second = {};
            submitted = false;
            completion.reset();
            native_reason.reset();
        }
        if (available && !submitted && awaiting()) {
            // Mark submission before calling native code; even a synchronous completion is handled on a later pass.
            {
                std::lock_guard lock(groups_mutex_);
                group->second.submitted = true;
            }

            const auto result = backend_.request(backend_.context, lobby);
            if (!result) {
                for (const auto &request : requests_) {
                    if (request->lobby.value == lobby && request->phase == RequestPhase::Pending) {
                        read_failed(*request, "native request", result.error());
                        break;
                    }
                }
                complete(lobby, false);
            }
            submitted = true;
        } else if (completion) {
            if (!*completion) {
                if (native_reason) {
                    for (const auto &request : requests_) {
                        if (request->lobby.value == lobby && request->phase == RequestPhase::Pending) {
                            if (request->owner->runtime) {
                                const auto text = std::format("Lobby {} key '{}': Galaxy could not complete the native read request "
                                                              "(reason code {}); pending reads receive Failed",
                                                              request->lobby.value, request->cache->key, *native_reason);

                                request->owner->runtime->reporting().write(request->owner, FC_LOG_WARNING, fc::detail::text(text));
                            }
                            break;
                        }
                    }
                }
                for (const auto &request : requests_) {
                    if (request->lobby.value == lobby && request->phase == RequestPhase::Pending) {
                        finish(*request, LobbyResult::Failed);
                    }
                }
            } else {
                for (const auto &entry : cache_) {
                    if (entry->lobby.value != lobby || entry->readers == 0) {
                        continue;
                    }

                    // Ready or active callbacks still refer to this observation; wait for delivery before replacing it.
                    const bool waiting_delivery = std::ranges::any_of(requests_, [&](const auto &request) {
                        return request->cache == entry.get() &&
                               (request->phase == RequestPhase::Ready || request->phase == RequestPhase::Delivering);
                    });
                    if (waiting_delivery) {
                        continue;
                    }

                    const auto read = backend_.read(backend_.context, lobby, entry->key);
                    if (!read) {
                        bool reported{};
                        for (const auto &request : requests_) {
                            if (request->cache == entry.get() && request->phase == RequestPhase::Pending) {
                                if (!reported) {
                                    read_failed(*request, "native read", read.error());
                                    reported = true;
                                }
                                finish(*request, LobbyResult::Failed);
                            }
                        }
                        continue;
                    }

                    // Release the previous bytes before decoding, so the reserved payload space covers only one observation.
                    entry->observation.reset();
                    auto decoded = decode(*read, value_limit_);
                    entry->observation = Observation{decoded.result, decoded.version, std::move(decoded.value), now};
                    entry->used = ++cache_use_;
                    for (const auto &request : requests_) {
                        if (request->cache == entry.get() && request->phase == RequestPhase::Pending) {
                            request->phase = RequestPhase::Ready;
                        }
                    }
                }
            }
        }
        if (!awaiting() && (!submitted || completion)) {
            std::lock_guard lock(groups_mutex_);
            used_ -= sizeof(std::pair<const std::uint64_t, Group>) + 4 * sizeof(void *);
            group = groups_.erase(group);
        } else {
            ++group;
        }
    }

    // Losing native access or ownership clears the last-write record but keeps accepted data for a later owned lobby.
    if (!available || !native.owned) {
        for (const auto &item : items_) {
            if (item->written) {
                item->written.reset();
            }
            const auto written_charge = item->desired ? item->desired->size() : 0;
            used_ -= item->written_charge - written_charge;
            item->written_charge = written_charge;
            item->written_lobby = {};
        }
        return;
    }

    for (const auto &item : items_) {
        if (item->owner->state.load(std::memory_order_acquire) != PatchState::Installed) {
            continue;
        }

        // Values must be written again when this process owns a different lobby.
        if (item->written_lobby.value != native.owned) {
            if (item->written) {
                item->written.reset();
            }
            const auto written_charge = item->desired ? item->desired->size() : 0;
            used_ -= item->written_charge - written_charge;
            item->written_charge = written_charge;
            item->written_lobby = {};
        }

        // Skip unchanged values and limit each item's native writes to one attempt per second.
        if ((!item->desired && !item->withdraw) ||
            (item->desired && !item->failure && item->written_lobby.value == native.owned && item->written == item->desired) ||
            (item->attempted && now - *item->attempted < std::chrono::seconds{1})) {
            continue;
        }

        if (!item->desired && !item->failure && item->written_lobby.value == native.owned && !item->written) {
            item->withdraw = false;
            continue;
        }

        const auto new_size = item->desired ? item->desired->size() : 0;
        // Copy the value before the native write so recording success needs no later allocation.
        auto written = item->desired;
        item->attempted = now;
        const auto result = backend_.write(backend_.context, native.owned, item->key,
                                           item->desired ? std::optional<std::string_view>{*item->desired} : std::nullopt);
        if (!result) {
            publication_failed(*item, result.error());
            continue;
        }

        used_ -= item->written_charge - new_size;
        item->written_charge = new_size;
        item->written = std::move(written);
        item->written_lobby = {native.owned};
        item->withdraw = false;
        item->failure.reset();
    }
}

void LobbyService::deliver() {
    // Mark only the ready requests present at entry; callbacks that request more work wait for the next pass.
    for (const auto &request : requests_) {
        if (request->phase == RequestPhase::Ready && request->owner->state.load(std::memory_order_acquire) == PatchState::Installed) {
            request->phase = RequestPhase::Delivering;
        }
    }

    // Find the next request after each callback: the callback may cancel requests or add a refresh.
    while (true) {
        const auto found = std::ranges::find_if(requests_, [](const auto &request) {
            return request->phase == RequestPhase::Delivering;
        });
        if (found == requests_.end()) {
            return;
        }

        // Keep the call's arguments before its callback can cancel and destroy the request token.
        auto &request = **found;
        auto *owner = request.owner;
        auto *capture = request.capture;
        const auto callback = request.callback;
        auto *selection = request.cache;
        FC_LobbyRecord record{request.lobby.value, static_cast<std::uint32_t>(request.failure), 0, {}, 0};
        if (selection) {
            // Keep the record bytes alive through this callback, even if it cancels its own request.
            ++selection->readers;
            const auto &observation = *selection->observation;
            record = record_for(request.lobby, observation.result, observation.version, observation.value, observation.observed,
                                request.version);
        }

        request.phase = RequestPhase::Delivered;
        request.capture = nullptr;
        request.callback = nullptr;
        release_selection(request.cache);

        PatchCallbackScope scope{owner};
        fc::invoke_patch_callback([&] {
            callback(capture, &record);
        });

        release_selection(selection);
    }
}

void LobbyService::discard(PatchRecord &owner) {
    for (const auto &request : requests_) {
        if (request->owner == &owner && request->phase != RequestPhase::Delivered) {
            release_selection(request->cache);
            request->capture = nullptr;
            request->callback = nullptr;
            request->phase = RequestPhase::Delivered;
        }
    }

    std::erase_if(items_, [&](const auto &item) {
        if (item->owner != &owner) {
            return false;
        }
        used_ -= sizeof(Item) + sizeof(std::unique_ptr<Item>) + item->key.size() + (item->desired ? item->desired->size() : 0) +
                 item->written_charge;
        return true;
    });
}

void LobbyService::issues(reporting::StatusText &status) const {
    for (const auto &item : items_) {
        if (item->failure) {
            if (!status.content().empty()) {
                status.line("");
            }
            if (!status.line(std::format("Lobby item {} '{}': Failed", qualified_patch_id(*item->owner), public_name(item->key))) ||
                !status.line("  Native write failed: " + *item->failure)) {
                return;
            }
        }
    }

    // Copy waiting lobby IDs under the listener lock, then format status after releasing it.
    std::vector<std::uint64_t> waiting;
    {
        std::lock_guard lock(groups_mutex_);
        for (const auto &[lobby, group] : groups_) {
            if (group.submitted && group.invalidated && !group.completion) {
                waiting.push_back(lobby);
            }
        }
    }
    for (const auto lobby : waiting) {
        if (!status.content().empty()) {
            status.line("");
        }
        if (!status.line(std::format("Lobby request for {}: Waiting", lobby)) ||
            !status.line("  Waiting for the previous native request to finish before starting new reads")) {
            return;
        }
    }
}

} // namespace fc::runtime
