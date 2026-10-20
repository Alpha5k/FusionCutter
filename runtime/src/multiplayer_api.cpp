#include "multiplayer.hpp"

#include "core_runtime.hpp"
#include "fatal_boundary.hpp"
#include "patches/patch_api.hpp"
#include "patches/patch_record.hpp"

#include <exception>
#include <utility>

namespace fc::runtime {
namespace {

// Returns the patch's multiplayer service, or null when the patch has no runtime.
MultiplayerService *service(void *identity) noexcept {
    const auto *owner = static_cast<PatchRecord *>(identity);
    return owner && owner->runtime ? &owner->runtime->multiplayer() : nullptr;
}

// Records the first registration error so prepare() fails even if the handler ignores it, then reports it to the caller.
void report_error(PatchRecord *owner, Diagnostic diagnostic, FC_ErrorSink sink) {
    if (owner && owner->api) {
        owner->api->fail(std::move(diagnostic));
        diagnostic = *owner->api->error();
    }

    if (sink.write) {
        sink.write(sink.context, {diagnostic.message.data(), static_cast<std::uint32_t>(diagnostic.message.size())},
                   diagnostic.windows_error);
    }
}

// Converts registration failures and exceptions into a failed prepare() and a C ABI diagnostic.
template <class F> std::uint32_t prepare(PatchRecord *owner, FC_ErrorSink sink, F &&function) {
    try {
        auto result = function();
        if (result) {
            return 1;
        }

        report_error(owner, std::move(result.error()), sink);
    } catch (const std::exception &exception) {
        report_error(owner, {owner ? qualified_patch_id(*owner) : std::string{}, exception.what()}, sink);
    } catch (...) {
        report_error(owner, {owner ? qualified_patch_id(*owner) : std::string{}, "Multiplayer registration raised an unknown exception"},
                     sink);
    }

    return 0;
}

std::uint64_t FC_CDECL host(void *owner) {
    const auto *value = service(owner);
    return value ? value->host().value : 0;
}

std::uint64_t FC_CDECL local(void *owner) {
    const auto *value = service(owner);
    return value ? value->local().value : 0;
}

// Calls the supplied writer synchronously for each current client, stopping when it returns false.
void FC_CDECL clients(void *owner, FC_PeerListSink sink) {
    const auto *value = service(owner);
    if (!value || !sink.write) {
        return;
    }

    try {
        const auto peers = value->clients();
        for (const auto peer : peers) {
            if (!sink.write(sink.context, &peer.value, 1)) {
                break;
            }
        }
    } catch (...) {
        fc::fatal_invariant("Could not copy the multiplayer client list");
    }
}

// Copies current peer details to the C ABI; presence flags distinguish a missing field from a valid zero value.
std::uint32_t FC_CDECL info(void *owner, std::uint64_t peer, FC_PeerInfo *out) {
    if (!out) {
        return 0;
    }

    *out = {};
    const auto *value = service(owner);
    const auto copied = value ? value->info({peer}) : std::nullopt;
    if (!copied) {
        return 0;
    }

    *out = {copied->slot.has_value(), copied->slot.value_or(0), copied->galaxy_id.has_value(), copied->galaxy_id.value_or(0)};
    return 1;
}

// Registers a feature during prepare() and returns its runtime handle, or reports why preparation failed.
std::uint32_t FC_CDECL register_feature(void *identity, const FC_FeatureDefinition *definition, FC_Feature **out, FC_ErrorSink sink) {
    auto *owner = static_cast<PatchRecord *>(identity);
    if (out) {
        *out = nullptr;
    }

    return prepare(owner, sink, [&]() -> std::expected<void, Diagnostic> {
        auto *value = service(owner);
        if (!value || !definition || !out || (!definition->id.data && definition->id.size)) {
            return std::unexpected(Diagnostic{owner ? qualified_patch_id(*owner) : std::string{},
                                              "Cannot register the multiplayer feature: missing runtime access or invalid arguments"});
        }

        auto result = value->feature(*owner, fc::detail::view(definition->id), definition->role);
        if (!result) {
            return std::unexpected(std::move(result.error()));
        }

        *out = *result;
        return {};
    });
}

std::uint32_t FC_CDECL agreement(FC_Feature *feature, std::uint64_t peer) {
    return static_cast<std::uint32_t>(feature ? feature->service->agreement(*feature, {peer}) : Agreement::Unavailable);
}

// Queues a complete message from the C ABI and returns its submission result.
std::uint32_t FC_CDECL send(FC_Feature *feature, std::uint64_t peer, FC_Bytes bytes) {
    if (!feature || (!bytes.data && bytes.size)) {
        return FC_SEND_UNAVAILABLE;
    }

    return static_cast<std::uint32_t>(
        feature->service->send(*feature, {peer}, {reinterpret_cast<const std::byte *>(bytes.data), bytes.size}));
}

std::uint32_t FC_CDECL max_payload(FC_Feature *feature) {
    return feature ? feature->service->max_payload(*feature) : 0;
}

// Fails communication with this peer for one feature and copies the supplied reason.
void FC_CDECL fail(FC_Feature *feature, std::uint64_t peer, FC_Text reason) {
    if (feature && (reason.data || !reason.size)) {
        try {
            feature->service->fail(*feature, {peer}, fc::detail::view(reason));
        } catch (...) {
            // The service sets the failure flags before copying the reason; those flags survive a failed allocation.
        }
    }
}

// Registers a peer callback and records any preparation error even when the caller ignores the result.
std::uint32_t FC_CDECL on_peer(FC_Feature *feature, void *capture, FC_PeerChange callback, FC_ErrorSink sink) {
    return prepare(feature ? feature->owner : nullptr, sink, [&]() -> std::expected<void, Diagnostic> {
        return feature ? feature->service->on_peer(*feature, capture, callback)
                       : std::unexpected(Diagnostic{{}, "Multiplayer feature is unavailable"});
    });
}

// Registers a message callback and records any preparation error even when the caller ignores the result.
std::uint32_t FC_CDECL on_message(FC_Feature *feature, void *capture, FC_FeatureMessage callback, FC_ErrorSink sink) {
    return prepare(feature ? feature->owner : nullptr, sink, [&]() -> std::expected<void, Diagnostic> {
        return feature ? feature->service->on_message(*feature, capture, callback)
                       : std::unexpected(Diagnostic{{}, "Multiplayer feature is unavailable"});
    });
}

} // namespace

const FC_MultiplayerApi *multiplayer_api_host() noexcept {
    static const FC_MultiplayerApi table{sizeof(FC_MultiplayerApi),
                                         &host,
                                         &local,
                                         &clients,
                                         &info,
                                         &register_feature,
                                         &agreement,
                                         &send,
                                         &max_payload,
                                         &fail,
                                         &on_peer,
                                         &on_message};
    return &table;
}

} // namespace fc::runtime
