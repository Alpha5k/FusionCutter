#pragma once

#include <fc/abi/common.h>

#include <stddef.h>

typedef struct lua_State lua_State;

// A Lua C closure receives its invoking stack and returns the number of results left there.
typedef int(FC_CDECL *FcLuaFunction)(lua_State *);
// A protected operation receives its Lua state and the caller's borrowed context.
typedef void(FC_CDECL *FcLuaProtectedFunction)(lua_State *, void *);

// Borrows Lua 5.0.2 functions and state field offsets verified for the selected executable.
// The table must outlive its VMs, closures and protected operations.
typedef struct FcLuaNative {
    // Byte offset of lua_State::top, the pointer just past the last occupied stack slot.
    size_t top_offset;
    // Byte offset of lua_State::l_G, the global state shared by a VM and its coroutines.
    size_t global_offset;
    // Byte offset of lua_State::stack, the base of the allocated stack used for recovery offsets.
    size_t stack_offset;

    // Stack indices use the invoking state's base; positive indices survive stack reallocation.
    // Reserve this many additional slots. May allocate, so call under native protection.
    int(FC_CDECL *checkstack)(lua_State *, int);
    // Return the number of occupied slots in the invoking stack.
    int(FC_CDECL *gettop)(lua_State *);
    // Resize the stack, filling added slots with nil; negative indices are relative to the current top.
    void(FC_CDECL *settop)(lua_State *, int);
    // Copy the indexed value to the stack top.
    void(FC_CDECL *pushvalue)(lua_State *, int);
    // Pop the top value into the indexed slot.
    void(FC_CDECL *replace)(lua_State *, int);

    // Return Lua's native type tag; checked adapters use it before converting a value.
    int(FC_CDECL *type)(lua_State *, int);
    // Apply Lua's truth test: nil and false are false; other values are true.
    int(FC_CDECL *toboolean)(lua_State *, int);
    // Read a native float32 number, converting numeric strings when possible.
    float(FC_CDECL *tonumber)(lua_State *, int);
    // Borrow string bytes while the value stays on Lua's stack or in its registry.
    // Converts numbers to strings; conversions may allocate and require native protection.
    const char *(FC_CDECL *tostring)(lua_State *, int);
    // Return the string's byte count, including embedded nulls; numeric conversions require native protection.
    size_t(FC_CDECL *strlen)(lua_State *, int);
    // Return a full or light userdata pointer, or NULL for another type.
    void *(FC_CDECL *touserdata)(lua_State *, int);
    // Return a pointer identifying a table, function, thread or userdata value, or NULL for another type.
    const void *(FC_CDECL *topointer)(lua_State *, int);

    // These operations may allocate or execute Lua and must run under native protection.
    // Push nil onto the invoking stack.
    void(FC_CDECL *pushnil)(lua_State *);
    // Push a native float32 number onto the invoking stack.
    void(FC_CDECL *pushnumber)(lua_State *, float);
    // Copy the specified number of string bytes, preserving embedded nulls.
    void(FC_CDECL *pushlstring)(lua_State *, const char *, size_t);
    // Pop the specified number of upvalues into a new C closure and push the closure.
    void(FC_CDECL *pushcclosure)(lua_State *, FcLuaFunction, int);
    // Push false for zero and true for any other integer.
    void(FC_CDECL *pushboolean)(lua_State *, int);
    // Push a borrowed pointer as light userdata; the caller retains ownership of its storage.
    void(FC_CDECL *pushlightuserdata)(lua_State *, void *);
    // Push a new empty table.
    void(FC_CDECL *newtable)(lua_State *);

    // Replace the top key with the indexed table's value, invoking metamethods when needed.
    void(FC_CDECL *gettable)(lua_State *, int);
    // Pop a key and value into the indexed table, invoking metamethods when needed.
    void(FC_CDECL *settable)(lua_State *, int);
    // Replace the top key with the indexed table's value, bypassing metamethods.
    void(FC_CDECL *rawget)(lua_State *, int);
    // Pop a key and value into the indexed table, bypassing metamethods.
    void(FC_CDECL *rawset)(lua_State *, int);
    // Push the indexed table's value for the integer key, bypassing metamethods.
    void(FC_CDECL *rawgeti)(lua_State *, int, int);
    // Pop a value into the indexed table at the integer key, bypassing metamethods.
    void(FC_CDECL *rawseti)(lua_State *, int, int);
    // Pop the current key and push the next key/value pair; return zero when iteration ends.
    int(FC_CDECL *next)(lua_State *, int);
    // Push the indexed value's metatable and return nonzero, or return zero when absent.
    int(FC_CDECL *getmetatable)(lua_State *, int);
    // Pop and assign a table or full userdata's metatable; nil removes it. Return zero for unsupported types.
    int(FC_CDECL *setmetatable)(lua_State *, int);
    // Allocate and push full userdata of the specified byte size; return its storage.
    void *(FC_CDECL *newuserdata)(lua_State *, size_t);

    // Call the stacked function with argument/result counts and an error-handler index; 0 means no handler.
    // Return Lua's status, leaving the requested results or an error value on the stack.
    int(FC_CDECL *pcall)(lua_State *, int, int, int);
    // Compile the named byte buffer and push its function, or an error value on failure; return its status.
    int(FC_CDECL *loadbuffer)(lua_State *, const char *, size_t, const char *);
    // Pop and retain a value in the indexed table; return the integer reference used to retrieve it.
    int(FC_CDECL *ref)(lua_State *, int);
    // Release an integer reference in the indexed table so its slot can be reused.
    void(FC_CDECL *unref)(lua_State *, int, int);
    // Raises the final Lua error only after the C++ callback dispatcher has returned.
    int(FC_CDECL *error)(lua_State *);

    // Run the C operation with Lua error recovery; old_top and errfunc are byte offsets from L->stack.
    // Return Lua's status; successful operations keep their stack results.
    int(FC_CDECL *protected_call)(lua_State *, FcLuaProtectedFunction, void *, ptrdiff_t old_top, ptrdiff_t errfunc);
} FcLuaNative;
