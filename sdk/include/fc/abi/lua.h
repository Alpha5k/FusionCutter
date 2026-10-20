#pragma once

#include <fc/abi/common.h>

#ifdef __cplusplus
extern "C" {
#endif

// Borrows a value on one invocation's Lua stack.
typedef struct FC_LuaValue {
    // Identifies the invocation that owns the stack slot.
    void *frame;
    // Positive stack index; zero represents nil.
    int32_t index;
} FC_LuaValue;

// Describes contiguous results kept on the invoking stack until the callback returns.
typedef struct FC_LuaRange {
    // Stack index of the first result when count is nonzero.
    int32_t first;
    uint32_t count;
} FC_LuaRange;

// Borrows a table iterator; callers keep this record and end iteration before the invocation returns.
typedef struct FC_LuaIterator {
    void *frame;
    int32_t table;
    // Entry stack slots; views expire when iteration_next reuses them or iteration_end clears them.
    int32_t key;
    int32_t value;
} FC_LuaIterator;

// Identifies a retained function in its owning VM.
typedef struct FC_LuaReference {
    // Opaque provider record; use reference_alive to check validity and reference_reset to release ownership.
    void *record;
    // Owning VM's lifetime ID, shared by its coroutines.
    uint64_t state;
} FC_LuaReference;

// Supplies the current invocation to a consumer callback without exposing private Lua headers.
typedef struct FC_LuaInvocation {
    void *frame;
    // Owning VM's lifetime ID, shared by its coroutines.
    uint64_t state;
    uint32_t argument_count;
} FC_LuaInvocation;

// Reports the actual Lua type using Fusion Cutter's ABI constants.
enum FC_LuaType {
    FC_LUA_NIL = 0,
    FC_LUA_BOOLEAN = 1,
    FC_LUA_NUMBER = 2,
    FC_LUA_STRING = 3,
    FC_LUA_TABLE = 4,
    FC_LUA_FUNCTION = 5,
    FC_LUA_LIGHT_USERDATA = 6,
    FC_LUA_USERDATA = 7,
    FC_LUA_THREAD = 8
};

// Selects a type or metatable inspection, or a strict value conversion.
enum FC_LuaRead {
    FC_LUA_READ_TYPE = 0,
    FC_LUA_READ_BOOLEAN = 1,
    FC_LUA_READ_NUMBER = 2,
    FC_LUA_READ_STRING = 3,
    FC_LUA_READ_POINTER = 4,
    FC_LUA_READ_USERDATA = 5,
    FC_LUA_READ_METATABLE = 6
};

// Supplies one value to push; only the fields selected by type are read.
typedef struct FC_LuaInput {
    uint32_t type;
    uint32_t boolean;
    // Converted to the game's float32 Lua number representation.
    double number;
    // String bytes, or the FC userdata tag when type is FC_LUA_USERDATA.
    FC_Text text;
    // Userdata bytes are copied into Lua-owned storage.
    FC_Bytes bytes;
    void *pointer;
} FC_LuaInput;

// Receives one inspection result; text and bytes borrow storage kept alive by the supplied value.
typedef struct FC_LuaReadResult {
    // Type, Boolean and Metatable use scalar; booleans are zero or one.
    uint32_t scalar;
    double number;
    FC_Text text;
    FC_Bytes bytes;
    void *pointer;
} FC_LuaReadResult;

// Selects how a declaration treats an existing target.
enum FC_LuaBindingMode {
    // Creates an absent target and missing parent tables; existing values remain intact.
    FC_LUA_ADD = 0,
    // Requires a function and retains it for original calls.
    FC_LUA_WRAP = 1,
    // Replaces an existing function.
    FC_LUA_REPLACE = 2
};

// Reports whether deferred work was accepted; only FC_LUA_ACCEPTED transfers capture ownership.
enum FC_LuaDispatchResult {
    FC_LUA_ACCEPTED = 0,
    FC_LUA_UNAVAILABLE = 1,
    FC_LUA_FULL = 2
};

#ifdef __cplusplus
}
#endif
