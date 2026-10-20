#include "service.hpp"

#include "service_records.hpp"

#include <Windows.h>

#include <algorithm>
#include <cassert>
#include <iterator>
#include <limits>
#include <utility>

namespace fc::core_plugin::lua_bindings {
namespace {
constexpr std::size_t kDeferredBytes = 64 * 1024;
}

Service::Service() {
    frame_operations(api_);
    api_.initialize = fc::bind<&Service::initialize>(*this);
    api_.bind = fc::bind<&Service::bind>(*this);
    api_.on_state = fc::bind<&Service::on_state>(*this);
    api_.run = fc::bind<&Service::run>(*this);
    api_.defer = fc::bind<&Service::defer>(*this);
    api_.reference_alive = fc::bind<&Service::reference_alive>(*this);
    api_.reference_reset = fc::bind<&Service::reference_reset>(*this);
    api_.retain = fc::bind<&Service::retain>(*this);
    api_.call_reference = fc::bind<&Service::call_reference>(*this);
}

Service::~Service() {
    // Call each consumer's destroy function on the control thread so its own DLL releases the capture.
    for (auto &consumer : consumers_) {
        for (auto *sequence : {&consumer.initializers, &consumer.bindings}) {
            for (auto &entry : *sequence) {
                if (entry->callback.destroy) {
                    entry->callback.destroy();
                }
            }
        }
    }
    for (auto &entry : deferred_) {
        if (entry->callback.destroy) {
            entry->callback.destroy();
        }
    }
    for (auto &entry : listeners_) {
        if (entry->callback.destroy) {
            entry->callback.destroy();
        }
    }
}

void Service::configure(FcLuaNative native) noexcept {
    native_ = native;
    fc_lua_configure_callbacks(&native_);
}

fc::Result<> Service::publish(fc::PrepareContext &context) {
    logger_ = context.log();
    return context.publish(fc::lua::detail::contract(), api_);
}

Service::Vm &Service::observe(lua_State *state) {
    const auto identity = fc_lua_vm(&native_, state);
    std::lock_guard lock{mutex_};
    const auto found = std::ranges::find_if(states_, [&](const auto &vm) {
        return vm->alive && vm->identity == identity;
    });
    if (found != states_.end()) {
        return **found;
    }

    // Coroutines share the VM's global state; assign a new ID when no live record has this identity.
    auto vm = std::make_unique<Vm>();
    vm->identity = identity;
    vm->state = state;
    assert(next_state_ != std::numeric_limits<std::uint64_t>::max());
    vm->id = ++next_state_;
    vm->thread = GetCurrentThreadId();
    auto &result = *vm;
    states_.push_back(std::move(vm));
    state_events_.push_back({result.id, true});
    return result;
}

void Service::setup(lua_State *state, std::string_view registered) noexcept {
    if (!state) {
        return;
    }

    // Keep the VM record alive while initializers and bindings run outside the lock.
    auto &vm = observe(state);
    {
        std::lock_guard lock{mutex_};
        ++vm.invoking;
    }

    apply(vm, state, registered);

    {
        std::lock_guard lock{mutex_};
        --vm.invoking;
    }
}

void Service::state_ending(lua_State *state) noexcept {
    if (!state) {
        return;
    }

    const auto identity = fc_lua_vm(&native_, state);
    std::lock_guard lock{mutex_};
    const auto found = std::ranges::find_if(states_, [&](const auto &vm) {
        return vm->alive && vm->identity == identity;
    });
    if (found == states_.end()) {
        return;
    }

    auto &vm = **found;
    vm.alive = false;
    state_events_.push_back({vm.id, false});

    // The game frees Lua's registry during teardown; invalidate tokens before their VM's memory is reused.
    for (auto &reference : references_) {
        if (reference->vm == &vm) {
            reference->vm = nullptr;
            reference->registry = -2;
        }
    }

    // Cancel waiting jobs now; an invoking job remains alive until its callback returns.
    for (auto &job : deferred_) {
        if (job->vm == &vm && !job->invoking) {
            job->vm = nullptr;
            job->complete = true;
        }
    }
}

std::uint32_t Service::initialize(fc::lua::detail::FrameCallback callback, fc::lua::detail::ErrorSink sink) noexcept {
    if (!detail::preparing(callback.invoke, sink)) {
        return 0;
    }
    if (!callback.invoke.bound || !callback.destroy) {
        detail::report(sink, "Lua initializer requires invoke and destroy callbacks");
        return 0;
    }

    auto declaration = std::make_unique<Declaration>();
    declaration->callback = callback;
    std::lock_guard lock{mutex_};
    add_declaration(std::move(declaration));
    return 1;
}

std::uint32_t Service::bind(const FC_Text *path, std::uint32_t count, std::uint32_t mode, fc::lua::detail::FrameCallback callback,
                            fc::lua::detail::ErrorSink sink) noexcept {
    if (!detail::preparing(callback.invoke, sink)) {
        return 0;
    }
    if (!count || !path || mode > FC_LUA_REPLACE || !callback.invoke.bound || !callback.destroy) {
        detail::report(sink, "Lua binding requires a nonempty path, a valid mode, and invoke and destroy callbacks");
        return 0;
    }

    // Copy the keys because the caller's path storage can expire after this registration call.
    auto declaration = std::make_unique<Declaration>();
    declaration->callback = callback;
    declaration->mode = mode;
    for (std::uint32_t i = 0; i < count; ++i) {
        if (path[i].size && !path[i].data) {
            detail::report(sink, "Lua binding path key has a nonzero length and a null text pointer");
            return 0;
        }
        declaration->path.emplace_back(fc::detail::view(path[i]));
    }

    // Reserve each binding path for one declaration, including a retired declaration still used by Lua closures.
    bool duplicate{};
    {
        std::lock_guard lock{mutex_};
        duplicate = std::ranges::any_of(consumers_, [&](const auto &consumer) {
            return std::ranges::any_of(consumer.bindings, [&](const auto &other) {
                return other->path == declaration->path;
            });
        });
        if (!duplicate) {
            add_declaration(std::move(declaration));
        }
    }

    if (duplicate) {
        detail::report(sink, "Another Lua binding already uses this path");
        return 0;
    }

    return 1;
}

void Service::add_declaration(std::unique_ptr<Declaration> declaration) {
    const auto owner = declaration->callback.invoke.owner;
    auto found = std::ranges::find(consumers_, owner, &Consumer::owner);
    if (found == consumers_.end()) {
        consumers_.push_back({.owner = owner});
        found = std::prev(consumers_.end());
    }

    auto &sequence = declaration->path.empty() ? found->initializers : found->bindings;
    sequence.push_back(std::move(declaration));
}

std::uint32_t Service::on_state(fc::lua::detail::StateCallback callback, fc::lua::detail::ErrorSink sink) noexcept {
    if (!detail::preparing(callback.invoke, sink)) {
        return 0;
    }
    if (!callback.invoke.bound || !callback.destroy) {
        detail::report(sink, "Lua state listener requires invoke and destroy callbacks");
        return 0;
    }

    auto listener = std::make_unique<StateListener>();
    listener->callback = callback;
    std::lock_guard lock{mutex_};
    listeners_.push_back(std::move(listener));
    return 1;
}

std::uint32_t Service::run(std::uint64_t id, fc::lua::detail::FrameInvocation callback, fc::lua::detail::ErrorSink sink) noexcept {
    // Hold the VM record through the callback, accepting only the thread that first observed this live VM.
    Vm *vm{};
    {
        std::lock_guard lock{mutex_};
        for (auto &entry : states_) {
            if (entry->id == id && entry->alive && entry->thread == GetCurrentThreadId()) {
                vm = entry.get();
                ++vm->invoking;
                break;
            }
        }
    }

    if (!vm) {
        detail::report(sink, "Lua state is unavailable or this is not its owning Lua thread");
        return 0;
    }

    std::uint32_t result{};
    {
        Frame frame{native_, vm->state, vm->id, 0};
        if (!callback.try_call(frame.invocation())) {
            detail::report(sink, "Lua callback consumer is inactive");
        } else {
            result = frame.result(sink);
        }
    }

    {
        std::lock_guard lock{mutex_};
        --vm->invoking;
    }
    return result;
}

std::uint32_t Service::defer(std::uint64_t id, fc::lua::detail::FrameCallback callback) noexcept {
    if (!callback.invoke.bound || !callback.destroy || !detail::active(callback.invoke)) {
        return FC_LUA_UNAVAILABLE;
    }

    std::lock_guard lock{mutex_};
    const auto found = std::ranges::find_if(states_, [&](const auto &vm) {
        return vm->alive && vm->id == id;
    });
    if (found == states_.end()) {
        return FC_LUA_UNAVAILABLE;
    }

    // Charge each record and its vector slot; keep the charge until control destroys the capture.
    constexpr std::size_t kRecordBytes = sizeof(Deferred) + sizeof(decltype(deferred_)::value_type);
    if (kRecordBytes > kDeferredBytes - deferred_bytes_) {
        return FC_LUA_FULL;
    }

    auto job = std::make_unique<Deferred>();
    job->vm = found->get();
    job->callback = callback;
    deferred_.push_back(std::move(job));
    deferred_bytes_ += kRecordBytes;
    return FC_LUA_ACCEPTED;
}

void Service::pump(lua_State *state) noexcept {
    if (!state) {
        return;
    }

    // Keep the VM record alive while declarations, reference releases and callbacks run outside the lock.
    auto &vm = observe(state);
    {
        std::lock_guard lock{mutex_};
        ++vm.invoking;
    }

    apply(vm, state);

    // Collect reset references under the lock, then remove them from Lua's registry on this thread.
    std::vector<int> releases;
    {
        std::lock_guard lock{mutex_};
        for (auto &reference : references_) {
            if (reference->vm == &vm && reference->reset && reference->registry >= 0) {
                releases.push_back(std::exchange(reference->registry, -2));
            }
        }
    }

    for (const auto reference : releases) {
        const auto result = fc_lua_release_registry(&native_, state, reference);
        if (!result.success) {
            const std::string reason{fc::detail::view(result.error)};
            fc_lua_restore(&native_, state, result.saved_top);
            logger_.error("Could not release Lua reference in state {}: {}", vm.id, reason);
        }
    }

    // Run at most 16 queued callbacks in one game update so remaining work waits for a later update.
    for (std::size_t count = 0; count < 16; ++count) {
        Deferred *job{};
        {
            std::lock_guard lock{mutex_};
            const auto found = std::ranges::find_if(deferred_, [&](const auto &entry) {
                return entry->vm == &vm && !entry->complete && !entry->invoking;
            });
            if (found == deferred_.end()) {
                break;
            }
            job = found->get();
            job->invoking = true;
        }

        detail::Failure failure;
        {
            Frame frame{native_, state, vm.id, 0};
            if (job->callback.invoke.try_call(frame.invocation())) {
                if (!frame.result(failure.sink())) {
                    logger_.error("Deferred Lua callback failed in state {}: {}", vm.id, failure.message);
                }
            }
        }
        // Mark completion after the frame restores Lua's stack; the control thread destroys the capture in its consumer DLL.
        {
            std::lock_guard lock{mutex_};
            job->invoking = false;
            job->complete = true;
            job->vm = nullptr;
        }
    }

    {
        std::lock_guard lock{mutex_};
        --vm.invoking;
    }
}

std::uint32_t Service::reference_alive(FC_LuaReference token) noexcept {
    std::lock_guard lock{mutex_};
    const auto found = std::ranges::find_if(references_, [&](const auto &entry) {
        return entry.get() == token.record;
    });
    return found != references_.end() && (*found)->id == token.state && (*found)->vm && !(*found)->reset ? 1u : 0u;
}

void Service::reference_reset(FC_LuaReference token) noexcept {
    std::lock_guard lock{mutex_};
    for (auto &entry : references_) {
        if (entry.get() == token.record && entry->id == token.state) {
            entry->reset = true;
            break;
        }
    }
}

std::uint32_t Service::retain(FC_LuaValue value, FC_LuaReference *out, fc::lua::detail::ErrorSink sink) noexcept {
    if (!value.frame || !out) {
        detail::report(sink, "Lua retain requires a current frame and output storage");
        return 0;
    }

    auto &frame = *static_cast<Frame *>(value.frame);
    Vm *vm{};
    {
        std::lock_guard lock{mutex_};
        for (auto &entry : states_) {
            if (entry->alive && entry->id == frame.state_id()) {
                vm = entry.get();
                break;
            }
        }
    }
    if (!vm || vm->thread != GetCurrentThreadId()) {
        detail::report(sink, "Lua reference requires a live VM on its owning thread");
        return 0;
    }

    // Save the function in the registry before issuing a token tied to this VM's ID.
    int registry{-2};
    if (!frame.retain_registry(value, &registry, sink)) {
        return 0;
    }

    auto reference = std::make_unique<Reference>();
    reference->vm = vm;
    reference->id = vm->id;
    reference->registry = registry;
    *out = {reference.get(), vm->id};
    std::lock_guard lock{mutex_};
    references_.push_back(std::move(reference));
    return 1;
}

std::uint32_t Service::call_reference(void *identity, FC_LuaReference token, const FC_LuaValue *arguments, std::uint32_t count,
                                      FC_LuaRange *out, fc::lua::detail::ErrorSink sink) noexcept {
    if (!identity) {
        detail::report(sink, "Retained Lua call requires a current frame");
        return 0;
    }

    // Match both the reference record and VM ID so a stale token cannot name a replacement VM.
    auto &frame = *static_cast<Frame *>(identity);
    int registry{-2};
    {
        std::lock_guard lock{mutex_};
        for (auto &entry : references_) {
            if (entry.get() == token.record && entry->id == token.state && token.state == frame.state_id() && entry->vm && !entry->reset) {
                registry = entry->registry;
                break;
            }
        }
    }

    if (registry < 0) {
        detail::report(sink, "Lua reference is stale or belongs to another VM");
        return 0;
    }

    return frame.call_registry(registry, arguments, count, out, sink);
}

void Service::control_update() noexcept {
    // Snapshot VM changes and current live IDs under the lock; state callbacks run after releasing it.
    std::vector<StateEvent> events;
    std::vector<std::uint64_t> live;
    {
        std::lock_guard lock{mutex_};
        events.swap(state_events_);
        for (const auto &vm : states_) {
            if (vm->alive) {
                live.push_back(vm->id);
            }
        }
    }

    // A listener's first notification reports current live VMs; subsequent updates report queued creation and teardown.
    for (auto &listener : listeners_) {
        if (!detail::active(listener->callback.invoke)) {
            listener->callback.destroy();
            listener->callback = {};
            continue;
        }

        if (listener->initial) {
            listener->initial = false;
            for (auto id : live) {
                (void)listener->callback.invoke.try_call(id, 1);
            }
        } else {
            for (auto event : events) {
                (void)listener->callback.invoke.try_call(event.state, event.alive ? 1u : 0u);
            }
        }
    }

    std::erase_if(listeners_, [](const auto &entry) {
        return !entry->callback.destroy;
    });

    // Ask the runtime about consumer activity outside the lock; reacquire it to retire declarations or cancel jobs.
    std::vector<Declaration *> declarations;
    std::vector<Deferred *> jobs;
    {
        std::lock_guard lock{mutex_};
        for (auto &consumer : consumers_) {
            for (auto *sequence : {&consumer.initializers, &consumer.bindings}) {
                for (auto &entry : *sequence) {
                    if (!entry->retired) {
                        declarations.push_back(entry.get());
                    }
                }
            }
        }
        for (auto &entry : deferred_) {
            if (!entry->complete && !entry->invoking) {
                jobs.push_back(entry.get());
            }
        }
    }

    for (auto *entry : declarations) {
        if (!detail::active(entry->callback.invoke)) {
            std::lock_guard lock{mutex_};
            entry->retired = true;
        }
    }
    for (auto *entry : jobs) {
        if (!detail::active(entry->callback.invoke)) {
            std::lock_guard lock{mutex_};
            if (!entry->invoking) {
                entry->complete = true;
                entry->vm = nullptr;
            }
        }
    }

    // Detach captures under the lock only after their active callbacks return; destroy them outside the lock.
    std::vector<fc::Function<void() noexcept>> destroy;
    std::vector<std::unique_ptr<Deferred>> completed;
    {
        std::lock_guard lock{mutex_};
        for (auto &consumer : consumers_) {
            for (auto *sequence : {&consumer.initializers, &consumer.bindings}) {
                for (auto &entry : *sequence) {
                    if (entry->retired && !entry->invoking && entry->callback.destroy) {
                        destroy.push_back(entry->callback.destroy);
                        entry->callback = {};
                    }
                }
            }
        }
        for (auto &entry : deferred_) {
            if (entry->complete && !entry->invoking) {
                completed.push_back(std::move(entry));
            }
        }
        std::erase_if(deferred_, [](const auto &entry) {
            return !entry;
        });

        // Reclaim reset references after registry release, and dead VMs after their last service operation returns.
        std::erase_if(references_, [](const auto &entry) {
            return entry->reset && entry->registry < 0;
        });
        std::erase_if(states_, [](const auto &entry) {
            return !entry->alive && !entry->invoking;
        });
        for (auto &vm : states_) {
            std::erase_if(vm->initialized, [](const auto *entry) {
                return entry->retired && !entry->invoking;
            });
        }

        // A retired declaration must remain alive while any surviving VM's Lua closure still points to it.
        for (auto &consumer : consumers_) {
            for (auto *sequence : {&consumer.initializers, &consumer.bindings}) {
                std::erase_if(*sequence, [&](const auto &entry) {
                    if (!entry->retired || entry->invoking || entry->callback.destroy) {
                        return false;
                    }
                    return std::ranges::none_of(states_, [&](const auto &vm) {
                        return std::ranges::any_of(vm->bindings, [&](const auto &binding) {
                            return binding->declaration == entry.get();
                        });
                    });
                });
            }
        }
        std::erase_if(consumers_, [](const auto &consumer) {
            return consumer.initializers.empty() && consumer.bindings.empty();
        });
    }

    for (auto callback : destroy) {
        callback();
    }
    for (auto &entry : completed) {
        // Keep completed records charged until their captures have actually been destroyed.
        entry->callback.destroy();
        entry.reset();
        std::lock_guard lock{mutex_};
        deferred_bytes_ -= sizeof(Deferred) + sizeof(decltype(deferred_)::value_type);
    }
}

} // namespace fc::core_plugin::lua_bindings
