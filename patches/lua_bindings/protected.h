#pragma once

#include "native.h"

#include <fc/abi/lua.h>

#ifdef __cplusplus
extern "C" {
#endif

// Reports a protected operation's status and the stack range containing its results.
typedef struct FcLuaResult {
    uint32_t success;
    // Stack depth before the operation, used to discard temporary values after a failure.
    int saved_top;
    // May borrow a Lua string; copy it before restoring saved_top after a failure.
    FC_Text error;
    FC_LuaRange values;
} FcLuaResult;

// Supplies the callback record borrowed by a Lua closure; the record must outlive the closure.
typedef struct FcLuaClosure {
    const FcLuaNative *native;
    void *context;
    // Completes C++ cleanup, then returns a result count or a negative value with the error on top of the stack.
    int(FC_CDECL *invoke)(void *, lua_State *);
} FcLuaClosure;

// Configures this process's function for reading closure upvalues before any closure is created.
void fc_lua_configure_callbacks(const FcLuaNative *native);

// Reads the pointer to Lua's shared global state, which identifies the owning VM.
void *fc_lua_vm(const FcLuaNative *native, lua_State *state);

// Discards values above a saved stack depth; top must be at or below the current depth.
void fc_lua_restore(const FcLuaNative *native, lua_State *state, int top);

// Appends a value from input, copying string or userdata bytes into Lua-owned storage.
FcLuaResult fc_lua_push(const FcLuaNative *native, lua_State *state, FC_LuaInput input);

// Reads kind from the indexed value using strict Lua types; index zero represents nil.
// String and userdata results borrow storage kept alive by the indexed value.
FcLuaResult fc_lua_read(const FcLuaNative *native, lua_State *state, int index, uint32_t kind, FC_Text tag, FC_LuaReadResult *out);

// Appends the value for key in table, using Lua access including __index; index zero supplies nil.
FcLuaResult fc_lua_get(const FcLuaNative *native, lua_State *state, int table, int key);

// Assigns value to key in table, using Lua access including __newindex; index zero supplies nil.
FcLuaResult fc_lua_set(const FcLuaNative *native, lua_State *state, int table, int key, int value);

// Appends the named global's value, using Lua access including __index.
FcLuaResult fc_lua_global_get(const FcLuaNative *native, lua_State *state, FC_Text name);

// Assigns value to the named global, using Lua access including __newindex; value zero supplies nil.
FcLuaResult fc_lua_global_set(const FcLuaNative *native, lua_State *state, FC_Text name, int value);

// Starts table iteration with two stack slots that keep the current key and value alive.
FcLuaResult fc_lua_iteration_begin(const FcLuaNative *native, lua_State *state, int table, FC_LuaIterator *out);

// Replaces the iterator's key and value slots with the next entry; has_entry is zero at the end.
FcLuaResult fc_lua_iteration_next(const FcLuaNative *native, lua_State *state, FC_LuaIterator *iterator, uint32_t *has_entry);

// Clears the iterator's slots and removes them when they are at the top, preserving later values.
FcLuaResult fc_lua_iteration_end(const FcLuaNative *native, lua_State *state, FC_LuaIterator *iterator);

// Calls the function selected by registry_reference when nonnegative, otherwise by the stack index function.
// Copies args first and appends all results; forward_incoming selects stack indices 1 through count instead.
FcLuaResult fc_lua_call(const FcLuaNative *native, lua_State *state, int function, int registry_reference, const FC_LuaValue *args,
                        uint32_t count, uint32_t forward_incoming);

// Loads source as a chunk named FC and executes it, appending all results.
FcLuaResult fc_lua_execute(const FcLuaNative *native, lua_State *state, FC_Text source);

// Retains function in the registry and writes its integer reference to out; use the live VM's Lua thread.
FcLuaResult fc_lua_retain_registry(const FcLuaNative *native, lua_State *state, int function, int *out);

// Releases a registry reference while its owning VM is live on its Lua thread.
FcLuaResult fc_lua_release_registry(const FcLuaNative *native, lua_State *state, int reference);

// Appends copies of values immediately, before a visitor's borrowed key or value slot can change.
FcLuaResult fc_lua_copy_values(const FcLuaNative *native, lua_State *state, const FC_LuaValue *values, uint32_t count);

// Appends copies of a contiguous stack range for Lua's C-function return values.
FcLuaResult fc_lua_copy_range(const FcLuaNative *native, lua_State *state, FC_LuaRange values);

// Copies error to the top of the stack for the C trampoline to raise after callback cleanup.
FcLuaResult fc_lua_prepare_error(const FcLuaNative *native, lua_State *state, FC_Text error);

// Appends a Lua closure that borrows closure as its upvalue; the record must outlive the Lua closure.
FcLuaResult fc_lua_closure(const FcLuaNative *native, lua_State *state, FcLuaClosure *closure);

#ifdef __cplusplus
}
#endif
