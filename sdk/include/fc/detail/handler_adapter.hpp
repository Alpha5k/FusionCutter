#pragma once

#include <fc/native.hpp>
#include <fc/settings.hpp>
#include <chrono>
#include <concepts>
#include <cstdint>
#include <exception>
#include <memory>
#include <string_view>
#include <utility>

namespace fc::detail {

// Call the error callback if one was provided.
// The callback must copy the message if it needs to keep it after returning.
inline void write_error(FC_ErrorSink error, std::string_view message, std::uint32_t windows_error = 0) noexcept {
    if (error.write != nullptr) {
        error.write(error.context, FC_Text{message.data(), static_cast<std::uint32_t>(message.size())}, windows_error);
    }
}

// The holder retains stable runtime access while allocating and destroying author state in this DLL.
template <class H> struct HandlerHolder {
    StageAccess access;
    // Captures outlive the handler destructor, which may invoke prepared originals or stop author workers.
    CallbackCaptures callback_captures;
    H handler;

    template <class... A> HandlerHolder(StageAccess value, A &&...arguments) : access(value), handler(std::forward<A>(arguments)...) {}
};

// Fills the C handler table with callbacks for H and transfers the holder to its destroy callback.
template <class H> void bind_handler(std::unique_ptr<HandlerHolder<H>> holder, FC_Handler *out) noexcept {
    *out = {};

    // This DLL destroys the holder; keep ownership here until the complete callback table is ready.
    out->context = holder.get();
    out->destroy = [](void *context) noexcept {
        delete static_cast<HandlerHolder<H> *>(context);
    };

    // Plan callbacks use the holder's callable storage; report planning exceptions through the C error sink.
    if constexpr (requires(H &h, Plan &p) { h.plan(p); }) {
        out->plan = [](void *context, FC_Plan *raw, FC_ErrorSink sink) noexcept -> std::uint32_t {
            try {
                auto &h = *static_cast<HandlerHolder<H> *>(context);
                Plan plan{raw, h.access, &h.callback_captures};
                h.handler.plan(plan);
                return 1;
            } catch (const std::exception &error) {
                report(sink, Error{error.what()});
            } catch (...) {
                report(sink, Error{"Planning raised an unknown exception"});
            }

            return 0;
        };
    }

    // Prepare uses the same callable storage; report returned errors and exceptions through the C error sink.
    if constexpr (requires(H &h, PrepareContext &p) {
                      { h.prepare(p) } -> std::same_as<Result<>>;
                  }) {
        out->prepare = [](void *context, FC_ErrorSink sink) noexcept -> std::uint32_t {
            try {
                auto &h = *static_cast<HandlerHolder<H> *>(context);
                PrepareContext prepare{h.access, &h.callback_captures};
                auto result = h.handler.prepare(prepare);
                if (result) {
                    return 1;
                }

                report(sink, result.error());
            } catch (const std::exception &error) {
                report(sink, Error{error.what()});
            } catch (...) {
                report(sink, Error{"Preparation raised an unknown exception"});
            }

            return 0;
        };
    }

    // Activation runs after installation; activation, update and status must be noexcept because a fault is fatal.
    if constexpr (requires(H &h, ActivateContext &a) { h.activate(a); }) {
        static_assert(noexcept(std::declval<H &>().activate(std::declval<ActivateContext &>())), "activate must be noexcept");
        out->activate = [](void *context) noexcept {
            auto &h = *static_cast<HandlerHolder<H> *>(context);
            ActivateContext activate{h.access};
            h.handler.activate(activate);
        };
    }

    // Convert the runtime's update timestamps to the steady-clock types expected by H::update.
    if constexpr (requires(H &h, const UpdateContext &u) { h.update(u); }) {
        static_assert(noexcept(std::declval<H &>().update(std::declval<const UpdateContext &>())), "update must be noexcept");
        out->update = [](void *context, std::int64_t now, std::int64_t elapsed) noexcept {
            auto &h = *static_cast<HandlerHolder<H> *>(context);
            // The runtime supplies steady-clock time and elapsed duration as nanoseconds.
            const UpdateContext update{std::chrono::steady_clock::time_point{
                                           std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::nanoseconds{now})},
                                       std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::nanoseconds{elapsed}),
                                       h.access};
            h.handler.update(update);
        };
    }

    // Give H::write_status a writer that remains valid until the callback returns.
    if constexpr (requires(const H &h, StatusContext &s) { h.write_status(s); }) {
        static_assert(noexcept(std::declval<const H &>().write_status(std::declval<StatusContext &>())), "write_status must be noexcept");
        out->status = [](void *context, FC_StatusWriter *writer) noexcept {
            auto &h = *static_cast<HandlerHolder<H> *>(context);
            StatusContext status{h.access, writer};
            std::as_const(h.handler).write_status(status);
        };
    }

    // The C table's destroy callback now owns the completed holder.
    holder.release();
}

// Returns 1 with author state and implemented callbacks, or 0 with an optional synchronous error and cleared out.
// out must be writable; typed handlers require their effective schema and values matching its count and order.
template <class H>
std::uint32_t FC_CDECL create_handler(void *schema, void *owner, const FC_Host *host, const FC_Value *values, std::uint32_t count,
                                      FC_Handler *out, FC_ErrorSink error) noexcept {
    *out = {};
    try {
        bind_reporting(host);
        const StageAccess access{owner, host};
        std::unique_ptr<HandlerHolder<H>> holder;
        // Reconstruct typed settings from schema-ordered scalar values before calling the author's constructor.
        if constexpr (requires { typename H::Settings; }) {
            using T = typename H::Settings;
            auto *binding = static_cast<const SettingsSchema *>(schema);
            const auto *model = static_cast<const SchemaModel<T> *>(binding->model.get());
            auto settings = model->construct({values, count});
            if (!settings) {
                report(error, settings.error());
                return 0;
            }
            // The schema's final validator may normalize the completed settings or reject construction.
            if (binding->complete) {
                auto completed = binding->complete(&*settings);
                if (!completed) {
                    report(error, completed.error());
                    return 0;
                }
            }
            holder = std::make_unique<HandlerHolder<H>>(access, CreateContext{access}, *settings);
        } else {
            // Prefer a constructor with runtime access when the handler declares one.
            if constexpr (std::constructible_from<H, const CreateContext &>) {
                holder = std::make_unique<HandlerHolder<H>>(access, CreateContext{access});
            } else {
                holder = std::make_unique<HandlerHolder<H>>(access);
            }
        }

        bind_handler(std::move(holder), out);
        return 1;
    } catch (const std::exception &exception) {
        write_error(error, exception.what());
    } catch (...) {
        write_error(error, "Handler construction raised an unknown exception");
    }
    return 0;
}

} // namespace fc::detail
