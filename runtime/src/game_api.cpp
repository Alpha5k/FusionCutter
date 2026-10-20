#include "game.hpp"

#include "core_runtime.hpp"
#include "patches/patch_api.hpp"
#include "patches/patch_record.hpp"

#include <exception>
#include <string_view>
#include <utility>

namespace fc::runtime {

namespace {

// Copies Game state into the caller's storage; missing access or unavailable state returns zero.
std::uint32_t FC_CDECL current(void *identity, FC_GameState *out) {
    if (!out) {
        return 0;
    }
    *out = {};
    const auto *owner = static_cast<PatchRecord *>(identity);
    if (!owner || !owner->runtime) {
        return 0;
    }

    const auto state = owner->runtime->game().current();
    if (!state) {
        return 0;
    }
    *out = {state->session, static_cast<std::uint32_t>(state->mode)};
    return 1;
}

// Keeps the first registration error so an ignored result still fails Prepare, then reports it synchronously.
void report_error(PatchRecord &owner, Diagnostic diagnostic, FC_ErrorSink sink) {
    owner.api->fail(std::move(diagnostic));
    const auto &error = *owner.api->error();
    if (sink.write) {
        sink.write(sink.context, {error.message.data(), static_cast<std::uint32_t>(error.message.size())}, error.windows_error);
    }
}

// Registers the patch's callback during Prepare and reports storage failures without unwinding across the C API.
std::uint32_t FC_CDECL on_change(void *identity, void *capture, FC_GameChange callback, FC_ErrorSink sink) {
    auto *owner = static_cast<PatchRecord *>(identity);
    if (!owner || !owner->runtime || !owner->api) {
        if (sink.write) {
            constexpr std::string_view message = "Game notifications are unavailable for this patch";
            sink.write(sink.context, {message.data(), static_cast<std::uint32_t>(message.size())}, 0);
        }
        return 0;
    }

    try {
        auto result = owner->runtime->game().on_change(*owner, capture, callback);
        if (result) {
            return 1;
        }
        report_error(*owner, std::move(result.error()), sink);
    } catch (const std::exception &exception) {
        report_error(*owner, {qualified_patch_id(*owner), exception.what()}, sink);
    } catch (...) {
        report_error(*owner, {qualified_patch_id(*owner), "Registering for Game changes raised an unknown exception"}, sink);
    }
    return 0;
}

} // namespace

const FC_GameApi *game_api_host() noexcept {
    static const FC_GameApi table{sizeof(FC_GameApi), &current, &on_change};
    return &table;
}

} // namespace fc::runtime
