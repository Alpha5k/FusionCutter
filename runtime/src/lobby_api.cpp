#include "lobby.hpp"

#include "core_runtime.hpp"
#include "patches/patch_api.hpp"
#include "patches/patch_record.hpp"

#include <exception>
#include <utility>

namespace fc::runtime {
namespace {

// Returns the patch's runtime Lobby service, or null when access is unavailable.
LobbyService *service(void *identity) noexcept {
    const auto *owner = static_cast<PatchRecord *>(identity);
    return owner && owner->runtime ? &owner->runtime->lobby() : nullptr;
}

// Keeps the first Prepare error so an ignored result still fails preparation, then reports it synchronously.
void report_error(PatchRecord *owner, Diagnostic error, FC_ErrorSink sink) {
    if (owner && owner->api && owner->state.load(std::memory_order_acquire) == PatchState::Preparing) {
        owner->api->fail(std::move(error));
        error = *owner->api->error();
    }

    if (sink.write) {
        sink.write(sink.context, {error.message.data(), static_cast<std::uint32_t>(error.message.size())}, error.windows_error);
    }
}

// Reports operation and allocation errors without unwinding across the C API; errors during Prepare still fail preparation.
template <class F> std::uint32_t operation(PatchRecord *owner, FC_ErrorSink sink, F &&function) {
    try {
        auto result = function();
        if (result) {
            return 1;
        }
        report_error(owner, std::move(result.error()), sink);
    } catch (const std::exception &exception) {
        report_error(owner, {owner ? qualified_patch_id(*owner) : std::string{}, exception.what()}, sink);
    } catch (...) {
        report_error(owner, {owner ? qualified_patch_id(*owner) : std::string{}, "Lobby operation raised an unknown exception"}, sink);
    }
    return 0;
}

// Registers an item during Prepare using the name's C byte view and returns its stable runtime handle.
std::uint32_t FC_CDECL register_item(void *identity, FC_Text name, std::uint32_t version, FC_LobbyItem **out, FC_ErrorSink sink) {
    auto *owner = static_cast<PatchRecord *>(identity);
    if (out) {
        *out = nullptr;
    }

    return operation(owner, sink, [&]() -> std::expected<void, Diagnostic> {
        auto *value = service(owner);
        if (!value || !out || (!name.data && name.size)) {
            return std::unexpected(Diagnostic{owner ? qualified_patch_id(*owner) : std::string{},
                                              "Cannot register Lobby item: patch access, output storage or the name view is invalid"});
        }
        auto result = value->item(*owner, fc::detail::view(name), version);
        if (!result) {
            return std::unexpected(std::move(result.error()));
        }
        *out = *result;
        return {};
    });
}

// Returns the process's current native lobby ID, or zero when unavailable.
std::uint64_t FC_CDECL current(void *owner) {
    const auto *value = service(owner);
    return value ? value->current().value : 0;
}

// Calls the C sink synchronously with a cached record, attributing the call to the requesting patch.
std::uint32_t FC_CDECL cached(void *identity, std::uint64_t lobby, FC_Text name, std::uint32_t version, void *capture, FC_LobbyRead read) {
    auto *owner = static_cast<PatchRecord *>(identity);
    auto *value = service(owner);
    if (!value || !read || (!name.data && name.size)) {
        return 0;
    }

    PatchCallbackScope scope{owner};
    return value->cached({lobby}, fc::detail::view(name), version, capture, read) ? 1u : 0u;
}

// Accepts a read request and returns its token; the callback runs in a later control pass.
std::uint32_t FC_CDECL refresh(void *identity, std::uint64_t lobby, FC_Text name, std::uint32_t version, void *capture, FC_LobbyRead read,
                               FC_LobbyRequest **out, FC_ErrorSink sink) {
    auto *owner = static_cast<PatchRecord *>(identity);
    if (out) {
        *out = nullptr;
    }

    return operation(owner, sink, [&]() -> std::expected<void, Diagnostic> {
        auto *value = service(owner);
        if (!value || !out || !read || (!name.data && name.size)) {
            return std::unexpected(Diagnostic{owner ? qualified_patch_id(*owner) : std::string{},
                                              "Cannot refresh Lobby data: patch access, output storage, name view or callback is invalid"});
        }
        auto result = value->refresh(*owner, {lobby}, fc::detail::view(name), version, capture, read);
        if (!result) {
            return std::unexpected(std::move(result.error()));
        }
        *out = *result;
        return {};
    });
}

// Copies the complete C byte view before accepting a new value; invalid input leaves the previous value unchanged.
std::uint32_t FC_CDECL publish(FC_LobbyItem *item, FC_Bytes bytes, FC_ErrorSink sink) {
    return operation(item ? item->owner : nullptr, sink, [&]() -> std::expected<void, Diagnostic> {
        if (!item || (!bytes.data && bytes.size)) {
            return std::unexpected(Diagnostic{{}, "Cannot publish Lobby data: the item handle or byte view is invalid"});
        }
        return item->service->publish(*item, {reinterpret_cast<const std::byte *>(bytes.data), bytes.size});
    });
}

// Schedules removal of this item's native value.
void FC_CDECL clear(FC_LobbyItem *item) {
    if (item) {
        item->service->clear(*item);
    }
}

// Releases an accepted request token, including after its callback has run.
void FC_CDECL cancel(FC_LobbyRequest *request) {
    if (request) {
        request->service->cancel(*request);
    }
}

} // namespace

const FC_LobbyApi *lobby_api_host() noexcept {
    static const FC_LobbyApi table{sizeof(FC_LobbyApi), &register_item, &current, &cached, &refresh, &publish, &clear, &cancel};
    return &table;
}

} // namespace fc::runtime
