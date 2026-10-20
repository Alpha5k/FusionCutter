#pragma once

#include <fc/base.hpp>
#include <fc/game.hpp>
#include <fc/lobby.hpp>
#include <fc/multiplayer.hpp>
#include <fc/patch_api.hpp>
#include <fc/reporting.hpp>

#include <chrono>

namespace fc {

// Supplies owner-bound logging during fallible handler construction on the control thread.
class CreateContext {
  public:
    explicit CreateContext(detail::StageAccess access) : access_(access) {}

    // Returns a logger attributed to this patch.
    Logger log() const {
        return Logger{access_};
    }

  private:
    detail::StageAccess access_;
};

// Reserves resources and connects declared providers before native installation and activation.
class PrepareContext {
  public:
    explicit PrepareContext(detail::StageAccess access, detail::CallbackCaptures *captures = nullptr)
        : access_(access), captures_(captures) {}

    // Returns Game access for this patch. Keep the handle as needed and register notifications during prepare().
    Game game() const noexcept {
        return Game{access_, captures_};
    }

    // Returns Multiplayer access for this patch. Keep the handle as needed and register features and callbacks during prepare().
    Multiplayer multiplayer() const noexcept {
        return Multiplayer{access_, captures_};
    }

    // Returns Lobby access for this patch. Keep the handle as needed and register items to publish during prepare().
    Lobby lobby() const noexcept {
        return Lobby{access_};
    }

    // Reserves a stable trace channel during fallible preparation; failed setup discards queued storage.
    Result<TraceChannel> trace(const TraceDefinition &definition) const {
        return detail::create_trace(access_, definition);
    }

    // Returns a logger attributed to this patch.
    Logger log() const {
        return Logger{access_};
    }

    // Copies this table during Prepare; bound objects must live for the provider's installed lifetime.
    template <class T> Result<> publish(const ApiContract<T> &contract, const T &table) {
        return detail::publish_api(access_.owner, access_.host, contract, table);
    }

    // Returns a table copy from an Installed declared dependency; bound provider objects live until exit.
    // Provider references use patch or plugin.patch, compared ASCII case-insensitively; failure rejects this consumer.
    template <class T> Result<T> get(std::string_view provider, const ApiContract<T> &contract) {
        return detail::get_api(access_.owner, access_.host, provider, contract);
    }

    // Captures a noexcept callable in this DLL and delivers once on the control thread after both patches activate.
    // No dependency is required; providers absent from selection never deliver, and late providers can deliver or report incompatibility.
    template <class T, class F> Result<> on_available(std::string_view provider, const ApiContract<T> &contract, F &&function) {
        return detail::on_api_available(access_.owner, access_.host, captures_, provider, contract, std::forward<F>(function));
    }

    // Creates a retained callback that checks the consumer's installed state before invoking the binding.
    // The bound object must outlive every installed callback use.
    template <class S> ConsumerCallback<S> callback(Function<S> function) const noexcept {
        return detail::consumer_callback(access_.owner, access_.host, function);
    }

    // Binds a noexcept member; object must survive all installed uses of the returned consumer callback.
    template <auto Method, class T> auto callback(T &object) const noexcept {
        return callback(bind<Method>(object));
    }

  private:
    detail::StageAccess access_;
    detail::CallbackCaptures *captures_;
};

// Runs on the control thread after installation; prepared native originals and state are already usable.
class ActivateContext {
  public:
    explicit ActivateContext(detail::StageAccess access) : access_(access) {}

    // Returns a logger attributed to this patch.
    Logger log() const {
        return Logger{access_};
    }

  private:
    detail::StageAccess access_;
};

// Supplies the serialized update time; delayed updates run once with their actual elapsed duration.
class UpdateContext {
  public:
    // Steady-clock sample taken by the runtime for this round of due updates.
    std::chrono::steady_clock::time_point now;
    // Time since this handler's preceding update, or its activation for the first update.
    std::chrono::steady_clock::duration elapsed;

    UpdateContext(std::chrono::steady_clock::time_point value, std::chrono::steady_clock::duration duration, detail::StageAccess access)
        : now(value), elapsed(duration), access_(access) {}

    // Returns a logger attributed to this patch.
    Logger log() const {
        return Logger{access_};
    }

  private:
    detail::StageAccess access_;
};

// Writes this installed handler's status synchronously on the control thread; the writer expires after the call.
class StatusContext {
  public:
    StatusContext(detail::StageAccess access, FC_StatusWriter *writer) : access_(access), writer_(writer) {}

    // Returns a logger attributed to this patch.
    Logger log() const {
        return Logger{access_};
    }

    // Appends a complete line; returns false if the bounded status writer cannot accept it.
    bool line(std::string_view value) const noexcept {
        return value.size() <= UINT32_MAX && writer_ && writer_->line && writer_->line(writer_->context, detail::text(value)) == 1;
    }

    // Formats and appends a line; returns false if formatting or submission fails.
    template <class... A> bool line(std::format_string<A...> format, A &&...arguments) const noexcept {
        try {
            return line(std::format(format, std::forward<A>(arguments)...));
        } catch (...) {
            return false;
        }
    }

  private:
    detail::StageAccess access_;
    FC_StatusWriter *writer_;
};
class Plan;
} // namespace fc
