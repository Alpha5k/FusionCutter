#include "core_runtime.hpp"

#include "fatal_boundary.hpp"
#include "patches/installation.hpp"
#include "patches/late_images.hpp"
#include "patches/patch_api.hpp"
#include "native/plan.hpp"
#include "../../plugins/core.hpp"
#include "../targets/game_context.hpp"
#include "../targets/galaxy.hpp"

#include <fc/reporting.hpp>

#include <fc/base.hpp>

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <exception>
#include <filesystem>
#include <format>
#include <string>
#include <thread>

namespace fc::runtime {

namespace {
constexpr std::array<std::string_view, 2> kGalaxyImages{"Galaxy.dll", "GalaxyPeer.dll"};

// Sends a borrowed diagnostic synchronously; the optional sink must copy any message it retains.
void report(FC_ErrorSink sink, const Diagnostic &error) {
    if (sink.write) {
        sink.write(sink.context, {error.message.data(), static_cast<std::uint32_t>(error.message.size())}, error.windows_error);
    }
}

} // namespace

void CoreRuntime::finish_reporting() noexcept {
    traces_.shutdown();
    reporting_.shutdown();
}

std::uint32_t CoreRuntime::start(FC_Role role, FC_ErrorSink error) {
    std::unique_lock lock{start_mutex_};
    if (role != FC_ROLE_CLIENT && role != FC_ROLE_SERVER) {
        lock.unlock();
        report(error, {"runtime", "Runtime start requires Client or Server mode"});
        return 0;
    }
    if (started_ && role != role_) {
        lock.unlock();
        report(error, {"runtime", "Runtime startup was already requested in a different launch mode"});
        return 0;
    }

    if (!started_) {
        started_ = true;
        role_ = role;
        try {
            // Each DLL has its own C++ statics; the named process guard detects a separately loaded copy.
            const auto name = L"Local\\FusionCutter.Runtime." + std::to_wstring(GetCurrentProcessId());
            const auto guard = CreateMutexW(nullptr, FALSE, name.c_str());
            const auto code = GetLastError();
            if (!guard) {
                throw Diagnostic{"runtime", "Could not create the runtime startup mutex", code};
            }
            if (code == ERROR_ALREADY_EXISTS) {
                CloseHandle(guard);
                throw Diagnostic{"runtime", "Another Fusion Cutter runtime copy has already begun startup in this process"};
            }

            process_guard_ = guard;
            std::thread{[this] {
                control_main();
            }}.detach();
        } catch (const Diagnostic &failure) {
            startup_result_ = std::unexpected(failure);
        } catch (const std::exception &exception) {
            startup_result_ = std::unexpected(Diagnostic{"runtime", exception.what()});
        }
    }

    // Waiting releases the startup lock so the control thread can publish the shared result.
    start_finished_.wait(lock, [this] {
        return startup_result_.has_value();
    });
    const auto result = *startup_result_;
    // Invoke caller-owned error reporting outside the startup lock.
    lock.unlock();
    if (!result) {
        report(error, result.error());
    }
    return result ? 1u : 0u;
}

void CoreRuntime::finish_start(std::expected<void, Diagnostic> result) {
    // Publish the complete result under the same lock used by startup waiters before waking them.
    {
        std::lock_guard lock{start_mutex_};
        startup_result_ = std::move(result);
    }

    start_finished_.notify_all();
}

void CoreRuntime::initialize() {
    if (const auto error = validate_image_catalog(catalog_)) {
        throw *error;
    }

    auto paths = runtime_configuration_paths();
    if (!paths) {
        throw paths.error();
    }
    configuration_paths_ = std::move(*paths);

    facts_.role = role_;
    std::array<wchar_t, 32768> executable{};
    const auto size = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (size && size < executable.size()) {
        facts_.executable_basename = path_text(std::filesystem::path{std::wstring_view{executable.data(), size}}.filename());
    }

    // Bring reporting online before reading configuration or loading plugins so startup failures can be recorded.
    if (!crash_.install(configuration_paths_.directory, facts_)) {
        throw Diagnostic{"runtime", "Could not initialize crash reporting", GetLastError()};
    }
    fc::install_fatal_handler(this, &fatal_handler);
    reporting_.start(facts_, configuration_paths_.directory);

    auto settings = load_runtime_configuration(configuration_paths_);
    if (!settings) {
        throw settings.error();
    }
    settings_ = *settings;
    reporting_.configure(settings->log_level);
    traces_.configure(settings->trace_file_bytes, configuration_paths_.directory, &reporting_);

    // Admit the core plugin and external DLLs, then resolve their image selectors against the catalog.
    auto discovered = discover_plugins(configuration_paths_.directory / "plugins");
    if (!discovered) {
        throw discovered.error();
    }
    const std::array embedded{DefinitionCandidate{"Core", fc::core_plugin::core_plugin()}};
    auto admitted = load_plugins(*discovered, embedded);
    session_ = resolve_patches(std::move(admitted), catalog_, role_);

    // Observe the executable and DLLs referenced by valid patch declarations.
    std::vector<const ResolvedPatch *> resolved;
    for (const auto &patch : session_.patches) {
        if (patch->resolved.definition) {
            resolved.push_back(&patch->resolved);
        }
    }

    images_ = observe_loaded_images(resolved, catalog_);

    for (const auto &image : images_) {
        if (image.executable && image.outcome && *image.outcome) {
            crash_.set_target((*image.outcome)->profile->id);
            break;
        }
    }

    for (const auto &image : images_) {
        if (image.outcome && *image.outcome) {
            publish_image_fact(**image.outcome);
        }
    }

    // Capture plugin settings once; later image checks select patches from these saved values.
    configure_patches(session_, image_observations(images_), configuration_paths_);
    for (auto warning : settings->warnings) {
        warning.level = FC_LOG_WARNING;
        session_.diagnostics.push_back(std::move(warning));
    }

    for (const auto &plugin : session_.plugins) {
        // Copy plugin identity and module facts so exception handling can read them from fixed reporting storage.
        CrashAnnotation annotation{CrashAnnotationKind::Plugin};
        annotation.label = fc::detail::view(plugin->admitted.definition->id);
        annotation.detail = fc::detail::view(plugin->admitted.definition->version);
        annotation.module = plugin->admitted.owner;

        // Plugin paths are stored as UTF-8; decode them explicitly for the native module lookup.
        const auto module =
            GetModuleHandleW(std::filesystem::path{std::u8string{plugin->admitted.owner.begin(), plugin->admitted.owner.end()}}.c_str());
        if (module) {
            annotation.address = reinterpret_cast<std::uintptr_t>(module);
            const auto base = reinterpret_cast<const std::byte *>(module);
            const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
            const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
            annotation.size = nt->OptionalHeader.SizeOfImage;
            annotation.timestamp = nt->FileHeader.TimeDateStamp;
        }
        crash_.publish({&annotation, 1});
    }

    // Give each patch its runtime facilities and a stable identity for crash reporting.
    for (const auto &patch : session_.patches) {
        patch->runtime = this;
        patch->api = std::make_unique<PatchApiReservations>(*patch);
        CrashAnnotation annotation{CrashAnnotationKind::Patch, patch.get()};
        const auto identity = qualified_patch_id(*patch);
        annotation.label = identity;
        crash_.publish({&annotation, 1});
    }

    // Select the game context hooks from the executable profile already identified for patches.
    game_observer_.resolved.owner = "Game context";
    game_observer_.runtime = this;
    game_observer_.internal_factory = game_context_factory(game_);
    game_observer_.reason = "The executable was not recognized";
    Diagnostic game_diagnostic{game_observer_.resolved.owner, {}, 0, FC_LOG_DEBUG};
    for (std::size_t index = 0; index != images_.size(); ++index) {
        if (!images_[index].executable) {
            continue;
        }

        const auto &outcome = images_[index].outcome;
        if (!outcome) {
            game_observer_.reason = outcome.error().message;
            game_diagnostic.windows_error = outcome.error().windows_error;
            game_diagnostic.level = FC_LOG_ERROR;
        } else if (!*outcome) {
            game_observer_.reason = "The executable could not be inspected";
            game_diagnostic.level = FC_LOG_ERROR;
        } else if (!supports_game_context((*outcome)->profile->id)) {
            game_observer_.reason = "Unsupported executable profile '" + std::string{(*outcome)->profile->id} + "'";
        } else {
            game_observer_.selection.state = SelectionState::Ready;
            game_observer_.selection.images.push_back({"game", (*outcome)->profile, index});
            game_observer_.state = PatchState::Planning;
            game_observer_.reason.clear();
        }
        break;
    }

    if (!game_observer_.reason.empty()) {
        game_diagnostic.message = game_observer_.reason;
        session_.diagnostics.push_back(std::move(game_diagnostic));
    }

    CrashAnnotation game_annotation{CrashAnnotationKind::Patch, &game_observer_};
    game_annotation.label = game_observer_.resolved.owner;
    crash_.publish({&game_annotation, 1});

    // Set the lobby limits before patches prepare their items; the Galaxy DLLs may load later.
    galaxy_observer_.resolved.owner = "Galaxy services";
    galaxy_observer_.runtime = this;
    galaxy_observer_.internal_factory = galaxy_factory(lobby_);
    select_galaxy_observer();
    CrashAnnotation galaxy_annotation{CrashAnnotationKind::Patch, &galaxy_observer_};
    galaxy_annotation.label = galaxy_observer_.resolved.owner;
    crash_.publish({&galaxy_annotation, 1});

    for (const auto &plugin : session_.plugins) {
        if (plugin->configuration) {
            for (auto warning : plugin->configuration->warnings) {
                warning.level = FC_LOG_WARNING;
                session_.diagnostics.push_back(std::move(warning));
            }
        }
    }

    std::string_view profile = "Unrecognized";
    for (const auto &image : images_) {
        if (image.executable && image.outcome && *image.outcome) {
            profile = (*image.outcome)->profile->id;
            break;
        }
    }
#if defined(_WIN64)
    constexpr auto architecture = "x64";
#else
    constexpr auto architecture = "x86";
#endif
    // The log writer's session header supplies build, executable and role; add the identified profile here.
    fc::Logger{{nullptr, &host_}}.info("Executable profile: {} ({})", profile, architecture);
    fc::Logger{{nullptr, &host_}}.debug("Runtime configuration: {}", path_text(configuration_paths_.directory / "FusionCutter.ini"));
    for (const auto &plugin : session_.plugins) {
        fc::Logger{{nullptr, &host_}}.debug("Accepted plugin '{}' version '{}' from {}", fc::detail::view(plugin->admitted.definition->id),
                                            fc::detail::view(plugin->admitted.definition->version),
                                            std::string_view{plugin->admitted.owner});
        const auto path =
            configuration_paths_.directory / ("FC." + std::string{fc::detail::view(plugin->admitted.definition->id)} + ".ini");
        std::error_code error;
        if (std::filesystem::exists(path, error)) {
            fc::Logger{{nullptr, &host_}}.debug("Plugin '{}' configuration: {}", fc::detail::view(plugin->admitted.definition->id),
                                                path_text(path));
        }
    }
    publish_selection();

    // Enforce fatal startup policy before installation and again after propagating installation failures.
    // Deliver service notifications only after the batch has completed installation and activation.
    publish_diagnostics();
    enforce_startup_policy(session_);
    const std::array private_owners{&game_observer_, &galaxy_observer_};
    install_ready_patches(session_, images_, {&host_, &hooks_, &crash_, &traces_, private_owners});
    refresh_selection(session_, image_observations(images_));
    publish_selection();
    enforce_startup_policy(session_);
    deliver_api_notifications(session_);
    service_control(std::chrono::steady_clock::now());
    publish_diagnostics();

    collect_status();
    next_status_ = next_update_time(std::chrono::steady_clock::now(), 1000);
}

void CoreRuntime::control_scan() {
    // Newly loaded DLLs can make waiting patches ready; cache their crash facts before installing native changes.
    const auto private_images = galaxy_observer_.state == PatchState::Waiting ? std::span<const std::string_view>{kGalaxyImages}
                                                                              : std::span<const std::string_view>{};
    const auto changed = process_awaited_images(session_, images_, private_images);
    if (!changed.empty()) {
        for (const auto index : changed) {
            if (images_[index].outcome && *images_[index].outcome) {
                publish_image_fact(**images_[index].outcome);
            }
        }

        select_galaxy_observer();
        publish_selection();
        const std::array private_owners{&game_observer_, &galaxy_observer_};
        install_ready_patches(session_, images_, {&host_, &hooks_, &crash_, &traces_, private_owners});
        refresh_selection(session_, image_observations(images_));
        publish_selection();
        enforce_startup_policy(session_);
        deliver_api_notifications(session_);
    }

    // Patch installation can delay this scan; sample update time after that work finishes.
    const auto now = std::chrono::steady_clock::now();
    run_due_updates(session_, now);
    service_control(now);
    publish_diagnostics();

    if (now >= next_status_) {
        collect_status();
        next_status_ = next_update_time(now, 1000);
    }
}

void CoreRuntime::select_galaxy_observer() {
    if (galaxy_observer_.state == PatchState::Installed || galaxy_observer_.state == PatchState::Failed ||
        (galaxy_observer_.state == PatchState::Disabled && !galaxy_observer_.reason.empty())) {
        return;
    }

    const auto main = std::ranges::find_if(images_, [](const auto &image) {
        return image.executable;
    });
    if (main == images_.end() || !main->outcome || !*main->outcome || !supports_galaxy((*main->outcome)->profile->id)) {
        galaxy_observer_.state = PatchState::Disabled;
        galaxy_observer_.reason = "Multiplayer messaging and lobby data do not support this executable";
        return;
    }

    if (galaxy_observer_.state == PatchState::Disabled) {
        lobby_.configure(1023, 4095);
    }

    galaxy_observer_.selection.images.clear();
    galaxy_observer_.selection.images.push_back({"game", (*main->outcome)->profile, static_cast<std::size_t>(main - images_.begin())});
    std::string waiting;

    // Add Galaxy DLLs to the shared image list so later scans can detect them and keep them loaded.
    for (const auto filename : kGalaxyImages) {
        if (std::ranges::none_of(images_, [&](const auto &image) {
                return fc::detail::equal_id(image.filename, filename);
            })) {
            images_.push_back({std::string{filename}, probe_loaded_image(filename, catalog_)});
            const auto &outcome = images_.back().outcome;
            if (outcome && *outcome) {
                publish_image_fact(**outcome);
            }
        }

        const auto found = std::ranges::find_if(images_, [&](const auto &image) {
            return fc::detail::equal_id(image.filename, filename);
        });
        const auto index = static_cast<std::size_t>(found - images_.begin());
        const auto &outcome = found->outcome;
        const bool galaxy = filename == "Galaxy.dll";

        // A loaded but incompatible DLL rejects this integration. Missing DLLs can still arrive on a later scan.
        if (!outcome || (*outcome && (galaxy ? !fc::detail::equal_id((*outcome)->profile->id, "Galaxy.59E63011")
                                             : (!fc::detail::equal_id((*outcome)->profile->id, "GalaxyPeer.59E6304A") &&
                                                !fc::detail::equal_id((*outcome)->profile->id, "GalaxyPeer.5BBE22A6"))))) {
            galaxy_observer_.state = PatchState::Disabled;
            galaxy_observer_.reason =
                "Unsupported " + std::string{filename} + ": " + (!outcome ? outcome.error().message : std::string{(*outcome)->profile->id});
            session_.diagnostics.push_back({galaxy_observer_.resolved.owner, galaxy_observer_.reason});
            return;
        }

        if (!*outcome) {
            waiting += (waiting.empty() ? "" : " and ") + std::string{filename};
        } else {
            galaxy_observer_.selection.images.push_back({galaxy ? "galaxy" : "peer", (*outcome)->profile, index});
        }
    }

    galaxy_observer_.selection.state = waiting.empty() ? SelectionState::Ready : SelectionState::Waiting;
    galaxy_observer_.state = waiting.empty() ? PatchState::Planning : PatchState::Waiting;
    galaxy_observer_.reason = waiting.empty() ? "" : "Waiting for " + waiting;
}

void CoreRuntime::service_control(std::chrono::steady_clock::time_point now) {
    if (galaxy_observer_.state == PatchState::Installed && galaxy_observer_.handler.update) {
        const auto elapsed = now - galaxy_observer_.last_update;
        galaxy_observer_.last_update = now;
        PatchCallbackScope scope{&galaxy_observer_};
        fc::invoke_patch_callback([&] {
            galaxy_observer_.handler.update(galaxy_observer_.handler.context,
                                            std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count(),
                                            std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
        });
    }

    // Refresh Galaxy first, then process lobby requests and deliver patch callbacks on this control thread.
    lobby_.pump(now);
    game_.deliver_changes();
    publish_game_state();
    multiplayer_.deliver();
    lobby_.deliver();
}

void CoreRuntime::control_main() noexcept {
    // Startup errors can return only before completion and before any installed or exposed native work.
    const auto requires_fatal = [this] {
        std::lock_guard lock{start_mutex_};
        return startup_result_.has_value() || game_observer_.state == PatchState::Installed ||
               (game_observer_.plan && game_observer_.plan->exposed()) || galaxy_observer_.state == PatchState::Installed ||
               (galaxy_observer_.plan && galaxy_observer_.plan->exposed()) || std::ranges::any_of(session_.patches, [](const auto &patch) {
                   return patch->state == PatchState::Installed || (patch->plan && patch->plan->exposed());
               });
    };

    try {
        initialize();
        finish_start({});

        for (;;) {
            control_scan();
            // Choose the earliest patch update, status refresh or next 50 ms image check as the wake-up time.
            const auto now = std::chrono::steady_clock::now();
            auto due = std::min(next_update_time(now, 50), next_status_);
            for (const auto &patch : session_.patches) {
                if (patch->state == PatchState::Installed && patch->handler.update) {
                    due = std::min(due, patch->next_update);
                }
            }

            std::this_thread::sleep_until(due);
        }
    } catch (const Diagnostic &diagnostic) {
        if (requires_fatal()) {
            fc::fatal_invariant(diagnostic.message, diagnostic.windows_error);
        }

        reporting_.diagnostic(diagnostic.message, diagnostic.windows_error, diagnostic.level);
        finish_start(std::unexpected(diagnostic));
    } catch (const std::exception &exception) {
        if (requires_fatal()) {
            fc::fatal_invariant(exception.what());
        }

        reporting_.diagnostic(exception.what(), 0, FC_LOG_ERROR);
        finish_start(std::unexpected(Diagnostic{"runtime", exception.what()}));
    } catch (...) {
        fc::fatal_invariant("Runtime control thread raised an unknown exception");
    }
}

void CoreRuntime::fatal_handler(void *context, std::string_view reason, std::uint32_t code) noexcept {
    auto &runtime = *static_cast<CoreRuntime *>(context);
    runtime.reporting_.diagnostic(reason, code, FC_LOG_ERROR);
    runtime.crash_.capture_fatal(reason, code, current_consumer());
}

std::uint32_t start_runtime(FC_Role role, FC_ErrorSink error) {
    // The owner and installed records intentionally remain alive until process exit.
    static auto *runtime = new CoreRuntime(known_image_profiles());
    return runtime->start(role, error);
}

} // namespace fc::runtime
