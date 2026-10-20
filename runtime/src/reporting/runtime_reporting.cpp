#include "../core_runtime.hpp"

#include "../fatal_boundary.hpp"
#include "patches/patch_api.hpp"
#include "native/plan.hpp"

#include <fc/base.hpp>
#include <fc/reporting.hpp>

#include <Windows.h>

#include <array>
#include <algorithm>
#include <chrono>
#include <format>

namespace fc::runtime {

namespace {

// Delivers the borrowed message and Windows error synchronously when the caller supplied an error sink.
void report(FC_ErrorSink sink, const Diagnostic &error) {
    if (sink.write) {
        sink.write(sink.context, {error.message.data(), static_cast<std::uint32_t>(error.message.size())}, error.windows_error);
    }
}

// Uses the outermost originating consumer through nested callbacks, falling back to the supplied owner outside them.
const PatchRecord *log_owner(void *owner) {
    return current_consumer() ? current_consumer() : static_cast<const PatchRecord *>(owner);
}

// Exposes the runtime severity filter through the ABI; invalid levels return 0.
std::uint32_t FC_CDECL enabled(void *context, void *, std::uint32_t level) {
    return level <= FC_LOG_DEBUG && static_cast<CoreRuntime *>(context)->reporting().enabled(static_cast<FC_LogLevel>(level)) ? 1u : 0u;
}

// Copies a valid packet into reporting with originating-consumer attribution; returns 1 when accepted.
std::uint32_t FC_CDECL submit(void *context, void *owner, std::uint32_t level, const FC_LogPacket *packet) {
    return packet && level <= FC_LOG_DEBUG &&
                   static_cast<CoreRuntime *>(context)->reporting().submit(log_owner(owner), static_cast<FC_LogLevel>(level), *packet)
               ? 1u
               : 0u;
}

// Copies text into reporting with originating-consumer attribution; returns 1 when accepted.
std::uint32_t FC_CDECL write(void *context, void *owner, std::uint32_t level, FC_Text message) {
    return level <= FC_LOG_DEBUG &&
                   static_cast<CoreRuntime *>(context)->reporting().write(log_owner(owner), static_cast<FC_LogLevel>(level), message)
               ? 1u
               : 0u;
}

// Creates a Preparing patch's channel; returns 1 with a retained channel, or 0 with null output and an error.
std::uint32_t FC_CDECL create_trace(void *context, void *identity, const FC_TraceDefinition *definition, void **out, FC_ErrorSink error) {
    if (out) {
        *out = nullptr;
    }

    const auto *owner = static_cast<PatchRecord *>(identity);
    if (!owner || owner->state != PatchState::Preparing || !definition || !out) {
        report(error, {"runtime", "Trace creation requires a patch in Preparing state, a definition and an output pointer"});
        return 0;
    }

    auto result = static_cast<CoreRuntime *>(context)->traces().create_channel(owner, *definition);
    if (!result) {
        report(error, result.error());
        return 0;
    }

    *out = *result;
    return 1;
}

// Reports whether the channel admits records; a null channel returns 0.
std::uint32_t FC_CDECL trace_enabled(void *, void *channel) {
    return channel && static_cast<TraceChannel *>(channel)->enabled() ? 1u : 0u;
}

// Copies a record into its channel and returns 1 when accepted; a null channel returns 0.
std::uint32_t FC_CDECL trace_write(void *, void *channel, FC_Bytes record) {
    return channel && static_cast<TraceChannel *>(channel)->write(record) ? 1u : 0u;
}

// Copies channel health to supplied output; a null channel supplies zero counters and flags.
void FC_CDECL trace_health(void *, void *channel, FC_TraceHealth *out) {
    if (out) {
        *out = channel ? static_cast<TraceChannel *>(channel)->health() : FC_TraceHealth{};
    }
}

// Records a fatal invariant and terminates with the outermost callback's attribution, or the supplied owner.
void FC_CDECL fatal(void *, void *owner, FC_Text message, std::uint32_t code) {
    PatchCallbackScope scope{static_cast<PatchRecord *>(owner)};
    fc::fatal_invariant(fc::detail::view(message), code);
}

// Names the current session mode; missing context remains distinct from an available context with no session.
std::string_view mode_name(std::optional<GameState> state) {
    if (!state) {
        return "Unavailable";
    }
    switch (state->mode) {
    case GameMode::None:
        return "None";
    case GameMode::Local:
        return "Local";
    case GameMode::Host:
        return "Host";
    case GameMode::Client:
        return "Client";
    case GameMode::Dedicated:
        return "Dedicated";
    }
    return "Unavailable";
}

// Indents every collected line, including line breaks supplied by custom status callbacks.
void append_lines(reporting::StatusText &status, std::string_view text, std::string_view indent) {
    while (!text.empty()) {
        const auto end = text.find('\n');
        const auto line = text.substr(0, end);
        if (!status.line(line.empty() ? std::string{} : std::string{indent} + std::string{line})) {
            return;
        }
        if (end == std::string_view::npos) {
            return;
        }
        text.remove_prefix(end + 1);
    }
}

// Keeps an issue's owner and outcome above its actual reason, including a known Windows error.
void append_issue(reporting::StatusText &issues, std::string_view heading, Diagnostic diagnostic) {
    if (!issues.content().empty()) {
        issues.line("");
    }
    issues.line(heading);
    diagnostic.owner.clear();
    issues.line("  " + diagnostic_text(diagnostic));
}

// Summarizes admitted plugins by outcome and lists only their explicitly disabled patches.
void append_plugins(reporting::StatusText &status, const SelectionSession &session) {
    status.line("");
    status.line("Plugins");
    for (const auto &plugin : session.plugins) {
        std::array<std::size_t, 5> counts{};
        std::string disabled;
        for (const auto &patch : session.patches) {
            if (patch->plugin != plugin.get()) {
                continue;
            }
            const auto state = patch->state.load(std::memory_order_acquire);
            if (state == PatchState::Disabled) {
                ++counts[1];
                disabled += (disabled.empty() ? "" : ", ") + std::string{fc::detail::view(patch->definition->id)};
            } else if (patch->selection.not_applicable && plugin->configuration) {
                ++counts[4];
            } else if (state == PatchState::Installed) {
                ++counts[0];
            } else if (state == PatchState::Waiting) {
                ++counts[2];
            } else if (state == PatchState::Failed) {
                ++counts[3];
            }
        }

        const auto &definition = *plugin->admitted.definition;
        auto line = std::format("  {} {}:", fc::detail::view(definition.id), fc::detail::view(definition.version));
        constexpr std::array names{"installed", "disabled", "waiting", "failed", "not applicable"};
        bool first = true;
        for (std::size_t i = 0; i != counts.size(); ++i) {
            if (counts[i]) {
                line += std::format("{}{} {}", first ? " " : ", ", counts[i], names[i]);
                first = false;
            }
        }
        status.line(line);
        if (!disabled.empty()) {
            status.line("    Disabled: " + disabled);
        }
    }
}

// Collects installed handlers synchronously and omits owners that supplied no visible text.
void append_patch_status(reporting::StatusText &status, const SelectionSession &session) {
    bool section{};
    for (const auto &patch : session.patches) {
        if (patch->state != PatchState::Installed || !patch->handler.status) {
            continue;
        }
        reporting::StatusText custom;
        PatchCallbackScope scope{patch.get()};
        auto writer = custom.writer();
        fc::invoke_patch_callback([&] {
            patch->handler.status(patch->handler.context, &writer);
        });
        if (custom.content().find_first_not_of(" \n") == std::string_view::npos) {
            continue;
        }
        if (!section) {
            status.line("");
            status.line("Patch Status");
            section = true;
        }
        status.line("  " + qualified_patch_id(*patch));
        append_lines(status, custom.content(), "    ");
    }
}

// Reports output configuration, actual failures and nonzero losses without implying an unused log file exists.
void append_diagnostics(reporting::StatusText &status, const reporting::LogStatus &log, const reporting::TraceStatus &trace) {
    status.line("");
    status.line("Diagnostics");
    status.line("  Log: " + path_text(log.path.filename()));
    status.line(std::string{"  Log level: "} + std::string{log_level_name(log.level)});
    if (log.output_failed) {
        status.line("  Log output: failed");
    }
    if (log.dropped) {
        status.line(std::format("  Lost log records: {}", log.dropped));
    }
    const auto state = trace.configured_disabled  ? "disabled"
                       : trace.output_failed      ? "failed"
                       : trace.file_limit_reached ? "stopped for size or disk limit"
                       : trace.path               ? "recording"
                                                  : "no data submitted";
    status.line(std::string{"  Tracing: "} + state);
    if (trace.path) {
        status.line("  Trace: " + path_text(trace.path->parent_path().filename() / trace.path->filename()));
    }
    if (trace.dropped) {
        status.line(std::format("  Trace channel records dropped: {}", trace.dropped));
    }
    if (trace.events_lost) {
        status.line(std::format("  ETW events lost: {}", trace.events_lost));
    }
    if (trace.buffers_lost) {
        status.line(std::format("  ETW buffers lost: {}", trace.buffers_lost));
    }
}

} // namespace

std::string diagnostic_text(const Diagnostic &diagnostic) {
    auto result = diagnostic.owner.empty() ? diagnostic.message : diagnostic.owner + ": " + diagnostic.message;
    if (!diagnostic.windows_error) {
        return result;
    }

    // Include the system's explanation when available, converting its UTF-16 text to UTF-8.
    result += " (Windows error " + std::to_string(diagnostic.windows_error);
    std::array<wchar_t, 2048> system{};
    const auto size = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, diagnostic.windows_error, 0,
                                     system.data(), static_cast<DWORD>(system.size()), nullptr);
    if (size) {
        auto message = std::wstring_view{system.data(), size};
        while (!message.empty() && (message.back() == '\r' || message.back() == '\n' || message.back() == ' ')) {
            message.remove_suffix(1);
        }
        const auto count = WideCharToMultiByte(CP_UTF8, 0, message.data(), static_cast<int>(message.size()), nullptr, 0, nullptr, nullptr);
        if (count) {
            std::string converted(static_cast<std::size_t>(count), '\0');
            WideCharToMultiByte(CP_UTF8, 0, message.data(), static_cast<int>(message.size()), converted.data(), count, nullptr, nullptr);
            result += ": " + converted;
        }
    }

    return result + ")";
}

CoreRuntime::CoreRuntime(std::span<const ImageProfile> catalog) : catalog_(catalog) {
    // These callback tables refer back to the runtime and remain available throughout its lifetime.
    reporting_table_ = {sizeof(FC_ReportingApi), this,           &enabled,     &submit,       &write,
                        &create_trace,           &trace_enabled, &trace_write, &trace_health, &fatal};
    host_ = {sizeof(FC_Host), native_api(), patch_api_host(), game_api_host(), multiplayer_api_host(), lobby_api_host(), &reporting_table_};
}

void CoreRuntime::publish_image_fact(const LoadedImage &image) {
    // Cache identity and address details now so crash reporting can use the recorded image information.
    CrashAnnotation annotation{CrashAnnotationKind::Module, nullptr, image.base, image.image_size};
    annotation.label = image.profile->id;
    annotation.module = image.filename;
    const auto base = reinterpret_cast<const std::byte *>(image.base);
    const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
    const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
    annotation.timestamp = nt->FileHeader.TimeDateStamp;
    crash_.publish({&annotation, 1});
}

void CoreRuntime::publish_diagnostics() {
    // Advance the cursor as diagnostics are submitted so later status passes report only new entries.
    while (diagnostic_count_ < session_.diagnostics.size()) {
        const auto &diagnostic = session_.diagnostics[diagnostic_count_++];
        const auto message = diagnostic_text(diagnostic);
        reporting_.write(nullptr, diagnostic.level, {message.data(), static_cast<std::uint32_t>(message.size())});
    }
}

void CoreRuntime::publish_selection() {
    for (const auto &patch : session_.patches) {
        const auto state = patch->state.load();
        if (state == PatchState::Installed || state == PatchState::Preparing) {
            continue;
        }

        std::string message;
        const bool not_applicable = state != PatchState::Disabled && patch->selection.not_applicable && patch->plugin->configuration;
        if (state == PatchState::Failed && !not_applicable) {
            // Admission, configuration and lifecycle errors already retain the original failure in the session.
            if (!patch->resolved.definition || patch->plugin->failure ||
                std::ranges::any_of(session_.diagnostics, [&](const Diagnostic &diagnostic) {
                    return diagnostic.owner == qualified_patch_id(*patch) && diagnostic.message == patch->reason;
                })) {
                continue;
            }
            std::string reason = patch->reason;
            if (!patch->selection.reasons.empty() && patch->reason == patch->selection.reasons.front().message) {
                reason.clear();
                for (auto diagnostic : patch->selection.reasons) {
                    diagnostic.owner.clear();
                    reason += (reason.empty() ? "" : "; ") + diagnostic_text(diagnostic);
                }
            }
            message = "Could not select patch: " + reason + "; patch will not run";
        } else {
            message = "Selection: " + std::string{not_applicable ? "Not applicable" : state_name(state)};
            if (!patch->reason.empty()) {
                message += "; " + patch->reason;
            }
            if (patch->selection.variant) {
                message += std::format("; variant {}", patch->selection.variant_index + 1);
                for (const auto &image : patch->selection.images) {
                    message += std::format("; image '{}' uses '{}' ({})", image.label, image.profile->id,
                                           images_[image.observation_index].filename);
                }
            }
            message += patch->failure_policy() == FC_FAILURE_FATAL ? "; failure policy Fatal" : "; failure policy Continue";
        }

        const auto previous = selection_messages_.find(patch.get());
        if (previous != selection_messages_.end() && previous->second == message) {
            continue;
        }
        if (previous == selection_messages_.end() && patch->plugin->configuration) {
            const auto &configuration = *patch->plugin->configuration;
            const auto index = static_cast<std::size_t>(patch->definition - patch->plugin->admitted.definition->patches);
            fc::Logger{{patch.get(), &host_}}.debug("Configuration: enabled={} from {}", patch->enabled,
                                                    control_source_name(configuration.controls[index].source));
        }
        selection_messages_[patch.get()] = message;
        if (state == PatchState::Failed && !not_applicable) {
            fc::Logger{{patch.get(), &host_}}.error("{}", std::string_view{message});
        } else {
            fc::Logger{{patch.get(), &host_}}.debug("{}", std::string_view{message});
        }
    }
}

void CoreRuntime::publish_game_state() {
    const auto game = game_.current();
    if (!game_logged_ || game.has_value() != logged_game_.has_value() ||
        (game && (game->session != logged_game_->session || game->mode != logged_game_->mode))) {
        fc::Logger{{nullptr, &host_}}.info("Game mode: {}; session {}", mode_name(game), game ? game->session : 0);
        logged_game_ = game;
        game_logged_ = true;
    }
}

void CoreRuntime::collect_status() {
    reporting::StatusText status;
#if defined(_WIN64)
    constexpr auto architecture = "x64";
#else
    constexpr auto architecture = "x86";
#endif
    status.line(std::format("Fusion Cutter {}", FC_VERSION_STRING));
    status.line(std::format("Architecture: {}", architecture));

    status.line(std::format("Started: {:%Y-%m-%d %H:%M:%S} UTC", std::chrono::floor<std::chrono::seconds>(facts_.started)));
    status.line("Directory: " + path_text(configuration_paths_.directory));
    status.line("");
    status.line("Game");
    status.line("  Executable: " + facts_.executable_basename);
    std::string_view build = "Unrecognized";
    for (const auto &image : images_) {
        if (image.executable && image.outcome && *image.outcome) {
            build = (*image.outcome)->profile->id;
            break;
        }
    }
    status.line(std::string{"  Build: "} + std::string{build});
    status.line(role_ == FC_ROLE_SERVER ? "  Role: Server" : "  Role: Client");
    status.line(std::string{"  Mode: "} + std::string{mode_name(game_.current())});

    // Mark only matching source diagnostics as represented; equal reasons from different files remain distinct issues.
    // Borrow their existing owners only for this control-thread collection call.
    reporting::StatusText issues;
    std::vector<const Diagnostic *> represented;
    const auto was_represented = [&](const Diagnostic &diagnostic) {
        return std::ranges::any_of(represented, [&](const Diagnostic *previous) {
            return previous->owner == diagnostic.owner && previous->message == diagnostic.message &&
                   previous->windows_error == diagnostic.windows_error;
        });
    };
    for (const auto &plugin : session_.plugins) {
        if (plugin->failure) {
            append_issue(issues, "Plugin " + std::string{fc::detail::view(plugin->admitted.definition->id)} + ": Failed",
                         {{}, diagnostic_text(*plugin->failure)});
            represented.push_back(&*plugin->failure);
        }
    }
    for (const auto &patch : session_.patches) {
        const auto state = patch->state.load(std::memory_order_acquire);
        if ((state != PatchState::Failed && state != PatchState::Waiting) || patch->plugin->failure ||
            (patch->selection.not_applicable && !patch->selection.reasons.empty() &&
             patch->reason == patch->selection.reasons.front().message)) {
            continue;
        }
        const auto owner = qualified_patch_id(*patch);
        const auto heading = owner + ": " + std::string{state_name(state)};
        if (!patch->selection.reasons.empty() && patch->reason == patch->selection.reasons.front().message) {
            if (!issues.content().empty()) {
                issues.line("");
            }
            issues.line(heading);
            for (const auto &reason : patch->selection.reasons) {
                represented.push_back(&reason);
                auto display = reason;
                display.owner.clear();
                issues.line("  " + diagnostic_text(display));
            }
        } else {
            const auto diagnostic = std::ranges::find_if(session_.diagnostics, [&](const Diagnostic &value) {
                return value.message == patch->reason && (value.owner == owner || value.owner == patch->plugin->admitted.owner);
            });
            append_issue(issues, heading, diagnostic == session_.diagnostics.end() ? Diagnostic{owner, patch->reason} : *diagnostic);
            if (diagnostic != session_.diagnostics.end()) {
                represented.push_back(&*diagnostic);
            }
        }
    }
    // Observer waiting states can still resolve after their DLLs arrive; installation failures retain their reasons.
    for (const auto *observer : {&game_observer_, &galaxy_observer_}) {
        if (!observer->reason.empty()) {
            const auto diagnostic = std::ranges::find_if(session_.diagnostics, [&](const Diagnostic &value) {
                return value.owner == observer->resolved.owner && value.message == observer->reason;
            });
            append_issue(issues, observer->resolved.owner,
                         diagnostic == session_.diagnostics.end() ? Diagnostic{observer->resolved.owner, observer->reason} : *diagnostic);
            if (diagnostic != session_.diagnostics.end()) {
                represented.push_back(&*diagnostic);
            }
        }
    }
    for (const auto &diagnostic : session_.diagnostics) {
        if (!was_represented(diagnostic)) {
            append_issue(issues, diagnostic.owner, diagnostic);
            represented.push_back(&diagnostic);
        }
    }
    multiplayer_.issues(issues);
    lobby_.issues(issues);
    if (!issues.content().empty()) {
        status.line("");
        status.line("Issues");
        append_lines(status, issues.content(), "  ");
    }

    append_plugins(status, session_);
    append_patch_status(status, session_);
    append_diagnostics(status, reporting_.status(), traces_.status());
    reporting_.publish_status(std::move(status).take());
}

} // namespace fc::runtime
