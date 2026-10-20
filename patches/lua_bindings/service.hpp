#pragma once

#include "native.h"

#include <fc/contexts.hpp>
#include <fc/lua/contract.hpp>
#include <fc/reporting.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string_view>
#include <vector>

namespace fc::core_plugin::lua_bindings {

// Tracks Lua VMs and manages consumer initializers, bindings, retained functions and queued callbacks.
class Service final {
  public:
    // Connects the Lua API table to this service; configure() must supply native calls before hooks are installed.
    Service();

    // Runs remaining callbacks' destroy functions on the control thread to release their captures.
    ~Service();

    Service(const Service &) = delete;
    Service &operator=(const Service &) = delete;

    // Copies the verified Lua function table during preparation, before native hooks can call this service.
    void configure(FcLuaNative native) noexcept;

    // Publishes the Lua API during preparation and keeps this patch's logger for service errors.
    fc::Result<> publish(fc::PrepareContext &);

    // Records the VM and applies active consumers' initializers and bindings on its Lua thread.
    // A nonempty registered name retries bindings under that global and saves its new function for Wrap.
    void setup(lua_State *, std::string_view registered = {}) noexcept;

    // Invalidates the VM and its retained work before native teardown can free or reuse the Lua memory.
    void state_ending(lua_State *) noexcept;

    // Records an existing VM if needed, releases reset references and runs up to 16 queued callbacks on its Lua thread.
    void pump(lua_State *) noexcept;

    // Reports VM creation and teardown and destroys finished or inactive consumers' captures on the control thread.
    void control_update() noexcept;

  private:
    struct Vm;
    struct Declaration;
    struct Consumer;
    struct Reference;
    struct Deferred;
    struct StateListener;
    struct Binding;

    // Finds the VM by its shared Lua global state, or assigns a new ID and records the current Lua thread.
    Vm &observe(lua_State *);

    // Adds a declaration to its consumer in first-registration order; the caller holds mutex_.
    void add_declaration(std::unique_ptr<Declaration>);

    // Runs each consumer's initializers once per VM before installing its bindings, in dependency order.
    // A nonempty registered name requests another installation attempt for bindings under that global.
    void apply(Vm &, lua_State *, std::string_view registered = {});

    // Installs a binding at its declared path, creating missing parent tables for Add and saving originals for Wrap.
    void install(Binding &, lua_State *);

    // Calls the consumer using the stack that entered the Lua closure, then leaves results or an error for the C trampoline.
    static int FC_CDECL invoke_binding(void *, lua_State *) noexcept;

    // Registers an initializer during consumer preparation, taking callback ownership on success.
    // bind() and on_state() use the same preparation and ownership rules.
    std::uint32_t initialize(fc::lua::detail::FrameCallback, fc::lua::detail::ErrorSink) noexcept;
    // Copies the literal path keys and registers an Add, Wrap or Replace binding at that path.
    std::uint32_t bind(const FC_Text *, std::uint32_t, std::uint32_t, fc::lua::detail::FrameCallback, fc::lua::detail::ErrorSink) noexcept;
    // Registers a callback for current live VMs and later VM creation and teardown notifications on the control thread.
    std::uint32_t on_state(fc::lua::detail::StateCallback, fc::lua::detail::ErrorSink) noexcept;

    // Invokes a borrowed callback synchronously when its VM is live on this Lua thread.
    std::uint32_t run(std::uint64_t, fc::lua::detail::FrameInvocation, fc::lua::detail::ErrorSink) noexcept;

    // Queues a callback for the named live VM; FC_LUA_ACCEPTED transfers ownership to this service.
    // Pending and completed records share a 64 KiB limit until the control thread destroys their captures.
    std::uint32_t defer(std::uint64_t, fc::lua::detail::FrameCallback) noexcept;

    // Reports whether the function reference is live in its owning VM and has not been reset.
    std::uint32_t reference_alive(FC_LuaReference) noexcept;

    // Invalidates the reference immediately and schedules removal from Lua's registry on the owning Lua thread.
    void reference_reset(FC_LuaReference) noexcept;

    // Saves the frame's function in its VM's registry and returns a token for calling it later.
    std::uint32_t retain(FC_LuaValue, FC_LuaReference *, fc::lua::detail::ErrorSink) noexcept;

    // Calls the retained function after checking that its token belongs to the current frame's live VM.
    std::uint32_t call_reference(void *, FC_LuaReference, const FC_LuaValue *, std::uint32_t, FC_LuaRange *,
                                 fc::lua::detail::ErrorSink) noexcept;

    // Queues a VM's creation or teardown notification for the control thread.
    struct StateEvent {
        std::uint64_t state;
        bool alive;
    };

    FcLuaNative native_{};
    fc::Logger logger_;
    fc::lua::detail::Api api_{};

    // Protects records shared by the Lua and control threads; release before calling consumers or native Lua.
    std::mutex mutex_;
    // Assigns a new ID even if Lua reuses an earlier VM's native address.
    std::uint64_t next_state_{};
    // Bytes charged for queued records and their vector slots; completed records count until capture destruction.
    std::size_t deferred_bytes_{};
    std::vector<std::unique_ptr<Vm>> states_;
    // First-registration order preserves consumer preparation order; each consumer owns its declarations.
    std::vector<Consumer> consumers_;
    std::vector<std::unique_ptr<Reference>> references_;
    std::vector<std::unique_ptr<Deferred>> deferred_;
    std::vector<std::unique_ptr<StateListener>> listeners_;
    std::vector<StateEvent> state_events_;
};

} // namespace fc::core_plugin::lua_bindings
