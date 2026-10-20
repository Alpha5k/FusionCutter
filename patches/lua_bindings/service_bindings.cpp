#include "service.hpp"

#include "service_records.hpp"

#include <algorithm>

namespace fc::core_plugin::lua_bindings {

void Service::apply(Vm &vm, lua_State *state, std::string_view registered) {
    // Snapshot the existing groups and pin their declarations while consumers and Lua run outside the lock.
    struct Declarations {
        std::vector<Declaration *> initializers;
        std::vector<Declaration *> bindings;
    };

    std::vector<Declarations> consumers;
    {
        std::lock_guard lock{mutex_};
        for (auto &consumer : consumers_) {
            auto &snapshot = consumers.emplace_back();
            const auto pin = [](const auto &sequence, auto &output) {
                for (auto &entry : sequence) {
                    if (!entry->retired) {
                        ++entry->invoking;
                        output.push_back(entry.get());
                    }
                }
            };
            pin(consumer.initializers, snapshot.initializers);
            pin(consumer.bindings, snapshot.bindings);
        }
    }

    // Finish each consumer's initializers before its bindings, even when bindings were registered first.
    for (const auto &consumer : consumers) {
        for (auto *entry : consumer.initializers) {
            {
                std::lock_guard lock{mutex_};
                if (std::ranges::find(vm.initialized, entry) != vm.initialized.end()) {
                    continue;
                }
            }
            if (!detail::active(entry->callback.invoke)) {
                continue;
            }

            // Record the attempt before invoking the initializer so failures and nested calls do not run it twice.
            {
                std::lock_guard lock{mutex_};
                if (std::ranges::find(vm.initialized, entry) != vm.initialized.end()) {
                    continue;
                }
                vm.initialized.push_back(entry);
            }

            Frame frame{native_, state, vm.id, 0};
            detail::Failure failure;
            if (entry->callback.invoke.try_call(frame.invocation()) && !frame.result(failure.sink())) {
                logger_.error("Lua initializer failed in state {}: {}", vm.id, failure.message);
            }
        }

        for (auto *entry : consumer.bindings) {
            if (!registered.empty() && entry->path.front() != registered) {
                continue;
            }

            Binding *binding{};
            {
                std::lock_guard lock{mutex_};
                const auto found = std::ranges::find_if(vm.bindings, [&](const auto &candidate) {
                    return candidate->declaration == entry;
                });
                if (found != vm.bindings.end()) {
                    binding = found->get();
                }
            }

            // Normal updates skip installed or failed bindings before probing consumer activity.
            if (registered.empty() && binding && (binding->installed || binding->conflicted)) {
                continue;
            }
            if (!detail::active(entry->callback.invoke)) {
                continue;
            }

            if (!binding) {
                std::lock_guard lock{mutex_};
                auto record = std::make_unique<Binding>();
                record->service = this;
                record->vm = &vm;
                record->declaration = entry;
                record->closure = {&native_, record.get(), &Service::invoke_binding};
                binding = record.get();
                vm.bindings.push_back(std::move(record));
            }

            // Native registration permits another attempt under this global and refreshes Wrap's original.
            if (!registered.empty()) {
                binding->conflicted = false;
            }
            install(*binding, state);
        }
    }

    {
        std::lock_guard lock{mutex_};
        for (const auto &consumer : consumers) {
            for (const auto *sequence : {&consumer.initializers, &consumer.bindings}) {
                for (auto *entry : *sequence) {
                    --entry->invoking;
                }
            }
        }
    }
}

void Service::install(Binding &binding, lua_State *state) {
    auto &declaration = *binding.declaration;
    auto &vm = *binding.vm;
    Frame frame{native_, state, vm.id, 0};
    detail::Failure failure;
    const auto type = [&](FC_LuaValue value) {
        FC_LuaReadResult result{};
        frame.read(value, FC_LUA_READ_TYPE, {}, &result, {});
        return result.scalar;
    };
    const auto string = [&](std::string_view value) {
        return frame.push({.type = FC_LUA_STRING, .text = fc::detail::text(value)});
    };
    const auto conflict = [&](std::string_view reason) {
        binding.conflicted = true;
        std::string path;
        for (const auto &key : declaration.path) {
            if (!path.empty()) {
                path += ".";
            }
            path += key;
        }
        logger_.error("Lua binding '{}' failed in state {}: {}", path, vm.id, reason);
    };
    // Lookup failures also return nil; check the frame's saved error before deciding that a path key is absent.
    const auto checked = [&] {
        if (frame.result(failure.sink())) {
            return true;
        }
        conflict(failure.message);
        return false;
    };

    // Follow literal path keys, creating missing parent tables for Add; Wrap and Replace wait for existing parents.
    FC_LuaValue parent{};
    FC_LuaValue target{};
    FC_LuaValue key{};
    if (declaration.path.size() == 1) {
        target = frame.global_get(fc::detail::text(declaration.path.front()));
    } else {
        parent = frame.global_get(fc::detail::text(declaration.path.front()));
        if (!checked()) {
            return;
        }
        if (type(parent) == FC_LUA_NIL) {
            if (declaration.mode != FC_LUA_ADD) {
                return;
            }
            parent = frame.push({.type = FC_LUA_TABLE});
            frame.global_set(fc::detail::text(declaration.path.front()), parent);
            if (!checked()) {
                return;
            }
        }
        if (type(parent) != FC_LUA_TABLE) {
            conflict("Parent value is not a table");
            return;
        }

        for (std::size_t i = 1; i + 1 < declaration.path.size(); ++i) {
            key = string(declaration.path[i]);
            auto next = frame.get(parent, key);
            if (!checked()) {
                return;
            }
            if (type(next) == FC_LUA_NIL) {
                if (declaration.mode != FC_LUA_ADD) {
                    return;
                }
                next = frame.push({.type = FC_LUA_TABLE});
                frame.set(parent, key, next);
                if (!checked()) {
                    return;
                }
            }
            if (type(next) != FC_LUA_TABLE) {
                conflict("Parent value is not a table");
                return;
            }
            parent = next;
        }

        key = string(declaration.path.back());
        target = frame.get(parent, key);
    }
    if (!checked()) {
        return;
    }

    // Recognize the existing wrapper first, then require nil for Add or a function for Wrap and Replace.
    const auto target_type = type(target);
    const void *identity{};
    if (target_type == FC_LUA_FUNCTION && !frame.identity(target, &identity, failure.sink())) {
        conflict(failure.message);
        return;
    }
    if (binding.identity && identity == binding.identity) {
        return;
    }
    if (declaration.mode == FC_LUA_ADD && target_type != FC_LUA_NIL) {
        conflict("Target value already exists");
        return;
    }
    if (declaration.mode != FC_LUA_ADD && target_type == FC_LUA_NIL) {
        return;
    }
    if (declaration.mode != FC_LUA_ADD && target_type != FC_LUA_FUNCTION) {
        conflict("Target value is not a function");
        return;
    }

    // Wrap retains the current function; all modes retain the new closure so Lua cannot reuse its identity pointer.
    int original{-2};
    if (declaration.mode == FC_LUA_WRAP && !frame.retain_registry(target, &original, failure.sink())) {
        conflict(failure.message);
        return;
    }

    const auto wrapper = frame.closure(&binding.closure);
    const void *wrapper_identity{};
    int wrapper_reference{-2};
    const bool created = frame.result(failure.sink()) && frame.identity(wrapper, &wrapper_identity, failure.sink()) &&
                         frame.retain_registry(wrapper, &wrapper_reference, failure.sink());
    if (created) {
        if (declaration.path.size() == 1) {
            frame.global_set(fc::detail::text(declaration.path.front()), wrapper);
        } else {
            frame.set(parent, key, wrapper);
        }
    }

    // Release temporary registry references if closure creation or assignment failed.
    if (!created || !frame.result(failure.sink())) {
        if (original >= 0) {
            const auto released = fc_lua_release_registry(&native_, state, original);
            if (!released.success) {
                logger_.error("Could not release the temporary wrapped Lua function in state {}: {}", vm.id,
                              fc::detail::view(released.error));
            }
        }
        if (wrapper_reference >= 0) {
            const auto released = fc_lua_release_registry(&native_, state, wrapper_reference);
            if (!released.success) {
                logger_.error("Could not release the temporary Lua wrapper in state {}: {}", vm.id, fc::detail::view(released.error));
            }
        }
        conflict(failure.message);
        return;
    }

    // Keep the previous original and wrapper until their replacement has been assigned successfully.
    if (binding.original >= 0) {
        const auto released = fc_lua_release_registry(&native_, state, binding.original);
        if (!released.success) {
            const std::string reason{fc::detail::view(released.error)};
            fc_lua_restore(&native_, state, released.saved_top);
            logger_.error("Could not release the previously wrapped Lua function in state {}: {}", vm.id, reason);
        }
    }

    binding.original = original;

    if (binding.wrapper >= 0) {
        const auto released = fc_lua_release_registry(&native_, state, binding.wrapper);
        if (!released.success) {
            const std::string reason{fc::detail::view(released.error)};
            fc_lua_restore(&native_, state, released.saved_top);
            logger_.error("Could not release the previous Lua wrapper in state {}: {}", vm.id, reason);
        }
    }

    binding.wrapper = wrapper_reference;
    binding.identity = wrapper_identity;
    binding.installed = true;
}

int FC_CDECL Service::invoke_binding(void *context, lua_State *state) noexcept {
    // Hold both records while the consumer runs; the control thread waits to delete the VM or destroy the capture.
    auto &binding = *static_cast<Binding *>(context);
    auto &service = *binding.service;
    auto &vm = *binding.vm;
    auto &declaration = *binding.declaration;
    fc::lua::detail::FrameInvocation callback{};
    {
        std::lock_guard lock{service.mutex_};
        ++vm.invoking;
        ++declaration.invoking;
        if (vm.alive && !declaration.retired) {
            callback = declaration.callback.invoke;
        }
    }

    int result{};
    {
        // Coroutines share vm.id but the frame always uses the actual stack that entered the C closure.
        Frame frame{service.native_, state, vm.id, static_cast<std::uint32_t>(service.native_.gettop(state)), binding.original};
        if (!callback.try_call(frame.invocation())) {
            frame.error(fc::detail::text("Lua binding consumer is inactive"));
        }
        result = frame.finish_binding();
    }

    // Release the records after C++ callback work ends, before the C trampoline returns results or raises the Lua error.
    {
        std::lock_guard lock{service.mutex_};
        --declaration.invoking;
        --vm.invoking;
    }
    return result;
}

} // namespace fc::core_plugin::lua_bindings
