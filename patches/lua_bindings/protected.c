#include "protected.h"

#include <limits.h>
#include <string.h>

enum {
    kRegistry = -10000,
    kGlobals = -10001,
    kUpvalue = -10002,
    kNativeNil = 0,
    kNativeBoolean = 1,
    kNativePointer = 2,
    kNativeNumber = 3,
    kNativeString = 4,
    kNativeTable = 5,
    kNativeFunction = 6,
    kNativeUserdata = 7,
    kNativeThread = 8
};

// The closure needs this function before it can read the record stored in its upvalue.
static void *(FC_CDECL *callback_userdata)(lua_State *, int);

// The shared registry maps this private key to FC's tagged userdata metatable.
static const char userdata_metatable_key = 0;

// Carries status and stack results through a protected Lua operation.
typedef struct Operation {
    const FcLuaNative *native;
    FcLuaResult result;
    int lua_status;
} Operation;

// Records copied tag and payload byte counts inside Lua-owned userdata.
typedef struct TaggedHeader {
    uint32_t tag_size;
    uint32_t payload_size;
} TaggedHeader;

// Records a fixed diagnostic whose storage stays valid after the Lua stack is restored.
static void fail(Operation *operation, const char *message) {
    operation->result.success = 0;
    operation->result.error.data = message;
    operation->result.error.size = (uint32_t)strlen(message);
}

// Reserves count additional stack slots under Lua protection, which also covers checkstack's allocations.
static int reserve(Operation *operation, lua_State *state, int count) {
    if (!operation->native->checkstack(state, count)) {
        fail(operation, "Lua could not reserve stack space");
        return 0;
    }
    return 1;
}

// Borrows the error string at the top of Lua's stack after checking its type.
static void native_error(Operation *operation, lua_State *state) {
    const FcLuaNative *native = operation->native;
    if (native->type(state, -1) == kNativeString) {
        const size_t size = native->strlen(state, -1);
        if (size <= UINT32_MAX) {
            operation->result.success = 0;
            operation->result.error.data = native->tostring(state, -1);
            operation->result.error.size = (uint32_t)size;
            return;
        }
    }
    fail(operation, "Lua raised an error without a readable string message");
}

// Runs a C operation under Lua protection and returns its status and stack results.
static FcLuaResult protect(lua_State *state, FcLuaProtectedFunction function, Operation *operation) {
    const FcLuaNative *native = operation->native;
    unsigned char *top;
    unsigned char *stack;
    int status;
    operation->result.success = 1;
    operation->result.saved_top = native->gettop(state);

    // Lua uses a byte offset to restore the top even if an allocation moves the stack.
    memcpy(&top, (const unsigned char *)state + native->top_offset, sizeof(top));
    memcpy(&stack, (const unsigned char *)state + native->stack_offset, sizeof(stack));
    status = native->protected_call(state, function, operation, top - stack, 0);
    if (status != 0 || operation->lua_status != 0) {
        native_error(operation, state);
    }
    return operation->result;
}

// Copies the indexed value onto the stack; index zero pushes nil.
static void push_index(const FcLuaNative *native, lua_State *state, int index) {
    if (index == 0) {
        native->pushnil(state);
    } else {
        native->pushvalue(state, index);
    }
}

// Maps Lua's native type constants to FC's public type values.
static uint32_t public_type(int type) {
    switch (type) {
    case kNativeBoolean:
        return FC_LUA_BOOLEAN;
    case kNativePointer:
        return FC_LUA_LIGHT_USERDATA;
    case kNativeNumber:
        return FC_LUA_NUMBER;
    case kNativeString:
        return FC_LUA_STRING;
    case kNativeTable:
        return FC_LUA_TABLE;
    case kNativeFunction:
        return FC_LUA_FUNCTION;
    case kNativeUserdata:
        return FC_LUA_USERDATA;
    case kNativeThread:
        return FC_LUA_THREAD;
    default:
        return FC_LUA_NIL;
    }
}

// Pushes the private userdata metatable, creating and registering it when create is nonzero.
static void userdata_metatable(Operation *operation, lua_State *state, int create) {
    const FcLuaNative *native = operation->native;
    native->pushlightuserdata(state, (void *)&userdata_metatable_key);
    native->rawget(state, kRegistry);
    if (native->type(state, -1) == kNativeTable || !create) {
        return;
    }

    native->settop(state, -2);
    native->newtable(state);

    // __metatable hides FC's private metatable from ordinary Lua code.
    native->pushlstring(state, "__metatable", 11);
    native->pushboolean(state, 0);
    native->rawset(state, -3);

    native->pushlightuserdata(state, (void *)&userdata_metatable_key);
    native->pushvalue(state, -2);
    native->rawset(state, kRegistry);
}

typedef struct PushOperation {
    Operation operation;
    FC_LuaInput input;
} PushOperation;

// Creates the selected value on Lua's stack, copying string and userdata bytes.
static void FC_CDECL push_value(lua_State *state, void *context) {
    PushOperation *value = (PushOperation *)context;
    Operation *operation = &value->operation;
    const FcLuaNative *native = operation->native;
    FC_LuaInput input = value->input;
    if (!reserve(operation, state, 5)) {
        return;
    }

    switch (input.type) {
    case FC_LUA_NIL:
        native->pushnil(state);
        break;
    case FC_LUA_BOOLEAN:
        native->pushboolean(state, input.boolean != 0);
        break;
    case FC_LUA_NUMBER:
        // The game stores Lua numbers as float32.
        native->pushnumber(state, (float)input.number);
        break;
    case FC_LUA_STRING:
        native->pushlstring(state, input.text.data ? input.text.data : "", input.text.size);
        break;
    case FC_LUA_TABLE:
        native->newtable(state);
        break;
    case FC_LUA_LIGHT_USERDATA:
        native->pushlightuserdata(state, input.pointer);
        break;
    case FC_LUA_USERDATA: {
        TaggedHeader *header;
        unsigned char *bytes;
        size_t size;
        if (input.text.size > SIZE_MAX - sizeof(TaggedHeader) || input.bytes.size > SIZE_MAX - sizeof(TaggedHeader) - input.text.size) {
            fail(operation, "Lua userdata header, tag and payload exceed SIZE_MAX bytes");
            return;
        }
        size = sizeof(TaggedHeader) + (size_t)input.text.size + input.bytes.size;
        userdata_metatable(operation, state, 1);

        // Store the tag and payload after a header that records their byte lengths.
        header = (TaggedHeader *)native->newuserdata(state, size);
        header->tag_size = input.text.size;
        header->payload_size = input.bytes.size;
        bytes = (unsigned char *)(header + 1);
        if (input.text.size) {
            memcpy(bytes, input.text.data, input.text.size);
        }
        if (input.bytes.size) {
            memcpy(bytes + input.text.size, input.bytes.data, input.bytes.size);
        }

        // Replace the temporary metatable with the finished userdata in the result slot.
        native->pushvalue(state, -2);
        native->setmetatable(state, -2);
        native->replace(state, operation->result.saved_top + 1);
        break;
    }
    default:
        fail(operation, "This Lua type cannot be constructed from copied input");
        return;
    }

    operation->result.values.first = operation->result.saved_top + 1;
    operation->result.values.count = 1;
}

typedef struct ReadOperation {
    Operation operation;
    int index;
    uint32_t kind;
    FC_Text tag;
    FC_LuaReadResult *out;
} ReadOperation;

// Reads the requested property or strictly typed value, borrowing Lua-owned string and userdata storage.
static void FC_CDECL read_value(lua_State *state, void *context) {
    ReadOperation *value = (ReadOperation *)context;
    Operation *operation = &value->operation;
    const FcLuaNative *native = operation->native;
    int type = value->index == 0 ? kNativeNil : native->type(state, value->index);
    FC_LuaReadResult *out = value->out;
    const char *mismatch;

    switch (value->kind) {
    case FC_LUA_READ_TYPE:
        out->scalar = public_type(type);
        return;
    case FC_LUA_READ_BOOLEAN:
        mismatch = "Expected a Lua boolean";
        if (type != kNativeBoolean) {
            break;
        }
        out->scalar = native->toboolean(state, value->index) != 0;
        return;
    case FC_LUA_READ_NUMBER:
        mismatch = "Expected a Lua number";
        if (type != kNativeNumber) {
            break;
        }
        out->number = (double)native->tonumber(state, value->index);
        return;
    case FC_LUA_READ_STRING: {
        size_t size;
        mismatch = "Expected a Lua string";
        if (type != kNativeString) {
            break;
        }
        size = native->strlen(state, value->index);
        if (size > UINT32_MAX) {
            fail(operation, "Lua string length exceeds the 32-bit text length limit");
            return;
        }
        out->text.data = native->tostring(state, value->index);
        out->text.size = (uint32_t)size;
        return;
    }
    case FC_LUA_READ_POINTER:
        mismatch = "Expected Lua light userdata";
        if (type != kNativePointer) {
            break;
        }
        out->pointer = native->touserdata(state, value->index);
        return;
    case FC_LUA_READ_METATABLE:
        if (!reserve(operation, state, 1)) {
            return;
        }
        out->scalar = value->index != 0 && native->getmetatable(state, value->index) != 0;
        native->settop(state, operation->result.saved_top);
        return;
    case FC_LUA_READ_USERDATA: {
        const TaggedHeader *header;
        const unsigned char *bytes;
        int matches;
        mismatch = "Expected Lua userdata created by FC";
        if (type != kNativeUserdata) {
            break;
        }
        if (!reserve(operation, state, 2)) {
            return;
        }

        // Verify the private metatable before interpreting the userdata as an FC header.
        if (!native->getmetatable(state, value->index)) {
            break;
        }
        userdata_metatable(operation, state, 0);
        matches = native->type(state, -1) == kNativeTable && native->topointer(state, -1) == native->topointer(state, -2);
        native->settop(state, operation->result.saved_top);
        if (!matches) {
            break;
        }

        header = (const TaggedHeader *)native->touserdata(state, value->index);
        bytes = (const unsigned char *)(header + 1);
        if (header->tag_size != value->tag.size || (value->tag.size && memcmp(bytes, value->tag.data, value->tag.size) != 0)) {
            fail(operation, "Lua userdata has a different FC type tag");
            return;
        }
        out->bytes.data = bytes + header->tag_size;
        out->bytes.size = header->payload_size;
        return;
    }
    default:
        fail(operation, "Unknown Lua inspection operation");
        return;
    }
    fail(operation, mismatch);
}

typedef struct TableOperation {
    Operation operation;
    int table;
    int key;
    int value;
    FC_Text name;
    int global;
    int write;
} TableOperation;

// Reads or writes global or table entries using ordinary Lua access.
static void FC_CDECL access_table(lua_State *state, void *context) {
    TableOperation *value = (TableOperation *)context;
    Operation *operation = &value->operation;
    const FcLuaNative *native = operation->native;
    int table;
    if (!reserve(operation, state, 3)) {
        return;
    }

    // Keep the key and any copied table alive while Lua may invoke a metamethod.
    if (value->global) {
        table = kGlobals;
        native->pushlstring(state, value->name.data ? value->name.data : "", value->name.size);
    } else {
        push_index(native, state, value->table);
        table = native->gettop(state);
        push_index(native, state, value->key);
    }

    if (value->write) {
        push_index(native, state, value->value);
        native->settable(state, table);
        native->settop(state, operation->result.saved_top);
    } else {
        native->gettable(state, table);
        if (!value->global) {
            // Use the copied table's slot for the result so the operation appends just one value.
            native->replace(state, table);
        }
        operation->result.values.first = operation->result.saved_top + 1;
        operation->result.values.count = 1;
    }
}

typedef struct IterationOperation {
    Operation operation;
    FC_LuaIterator *iterator;
    uint32_t *has_entry;
    int action;
} IterationOperation;

// Starts, advances or ends iteration using two reusable stack slots.
static void FC_CDECL iterate(lua_State *state, void *context) {
    IterationOperation *value = (IterationOperation *)context;
    Operation *operation = &value->operation;
    const FcLuaNative *native = operation->native;
    FC_LuaIterator *iterator = value->iterator;
    if (!reserve(operation, state, 2)) {
        return;
    }

    if (value->action == 0) {
        if (native->type(state, iterator->table) != kNativeTable) {
            fail(operation, "Lua iteration requires a table");
            return;
        }
        iterator->key = operation->result.saved_top + 1;
        iterator->value = operation->result.saved_top + 2;
        // Lua's next starts at a nil key; the second slot holds the current entry's value.
        native->pushnil(state);
        native->pushnil(state);
    } else if (value->action == 1) {
        // Fetch into temporary slots, then overwrite the iterator's key and value slots.
        native->pushvalue(state, iterator->key);
        *value->has_entry = native->next(state, iterator->table) != 0;
        if (*value->has_entry) {
            native->replace(state, iterator->value);
            native->replace(state, iterator->key);
        }
        native->settop(state, operation->result.saved_top);
    } else {
        // Clear the entry, preserving the indices of any values appended after the iterator's slots.
        native->pushnil(state);
        native->replace(state, iterator->key);
        native->pushnil(state);
        native->replace(state, iterator->value);
        if (operation->result.saved_top == iterator->value) {
            native->settop(state, iterator->key - 1);
        }
    }
}

typedef struct CallOperation {
    Operation operation;
    int function;
    int reference;
    const FC_LuaValue *args;
    uint32_t count;
    uint32_t forward_incoming;
    FC_Text source;
    int execute;
} CallOperation;

// Calls a function or loaded chunk; outer protection also catches failures while preparing the call.
static void FC_CDECL call_function(lua_State *state, void *context) {
    CallOperation *value = (CallOperation *)context;
    Operation *operation = &value->operation;
    const FcLuaNative *native = operation->native;
    uint32_t index;
    if (value->count > INT_MAX - 1) {
        fail(operation, "Lua argument count plus the function exceeds INT_MAX stack slots");
        return;
    }
    if (!reserve(operation, state, (int)value->count + 1)) {
        return;
    }

    if (value->execute) {
        operation->lua_status = native->loadbuffer(state, value->source.data ? value->source.data : "", value->source.size, "FC");
        if (operation->lua_status != 0) {
            return;
        }
    } else if (value->reference >= 0) {
        native->rawgeti(state, kRegistry, value->reference);
    } else {
        push_index(native, state, value->function);
    }

    // Copy arguments so the caller's values remain on the stack when pcall consumes its inputs.
    for (index = 0; index < value->count; ++index) {
        push_index(native, state, value->forward_incoming ? (int)index + 1 : value->args[index].index);
    }

    // Request all results; pcall leaves either those values or its error on the stack.
    operation->lua_status = native->pcall(state, (int)value->count, -1, 0);
    if (operation->lua_status != 0) {
        return;
    }

    operation->result.values.first = operation->result.saved_top + 1;
    operation->result.values.count = (uint32_t)(native->gettop(state) - operation->result.saved_top);
}

typedef struct RegistryOperation {
    Operation operation;
    int function;
    int reference;
    int *out;
} RegistryOperation;

// Retains or releases functions using Lua's registry reference functions and their shared freelist.
static void FC_CDECL registry(lua_State *state, void *context) {
    RegistryOperation *value = (RegistryOperation *)context;
    Operation *operation = &value->operation;
    const FcLuaNative *native = operation->native;
    if (!reserve(operation, state, 3)) {
        return;
    }

    if (value->out) {
        if (value->function == 0 || native->type(state, value->function) != kNativeFunction) {
            fail(operation, "Only a Lua function can be retained");
            return;
        }
        native->pushvalue(state, value->function);
        *value->out = native->ref(state, kRegistry);
    } else {
        native->unref(state, kRegistry, value->reference);
    }
    native->settop(state, operation->result.saved_top);
}

typedef struct CopyOperation {
    Operation operation;
    const FC_LuaValue *values;
    uint32_t count;
    int first;
    FC_Text error;
    int preparing_error;
} CopyOperation;

// Appends copies of selected results or the final callback error to the Lua stack.
static void FC_CDECL copy_values(lua_State *state, void *context) {
    CopyOperation *value = (CopyOperation *)context;
    Operation *operation = &value->operation;
    const FcLuaNative *native = operation->native;
    uint32_t index;
    if (value->count > INT_MAX) {
        fail(operation, "Lua result count exceeds INT_MAX stack slots");
        return;
    }
    if (!reserve(operation, state, value->preparing_error ? 1 : (int)value->count)) {
        return;
    }

    if (value->preparing_error) {
        native->pushlstring(state, value->error.data ? value->error.data : "", value->error.size);
    } else {
        // Copy now so later iterator visits cannot replace the selected return values.
        for (index = 0; index < value->count; ++index) {
            push_index(native, state, value->values ? value->values[index].index : value->first + (int)index);
        }
        operation->result.values.first = operation->result.saved_top + 1;
        operation->result.values.count = value->count;
    }
}

// Invokes the callback and returns its result count, or raises the error left on the stack.
static int FC_CDECL callback(lua_State *state) {
    FcLuaClosure *closure = (FcLuaClosure *)callback_userdata(state, kUpvalue);
    const int count = closure->invoke(closure->context, state);

    // invoke has completed C++ cleanup before Lua can leave this C function through its error handler.
    return count < 0 ? closure->native->error(state) : count;
}

typedef struct ClosureOperation {
    Operation operation;
    FcLuaClosure *closure;
} ClosureOperation;

// Creates a closure that borrows a callback record.
static void FC_CDECL closure_operation(lua_State *state, void *context) {
    ClosureOperation *value = (ClosureOperation *)context;
    Operation *operation = &value->operation;
    const FcLuaNative *native = operation->native;
    if (!reserve(operation, state, 1)) {
        return;
    }

    // The light userdata upvalue borrows the provider's retained callback record.
    native->pushlightuserdata(state, value->closure);
    native->pushcclosure(state, &callback, 1);
    operation->result.values.first = operation->result.saved_top + 1;
    operation->result.values.count = 1;
}

void fc_lua_configure_callbacks(const FcLuaNative *native) {
    callback_userdata = native->touserdata;
}

void *fc_lua_vm(const FcLuaNative *native, lua_State *state) {
    void *global;
    memcpy(&global, (const unsigned char *)state + native->global_offset, sizeof(global));
    return global;
}

void fc_lua_restore(const FcLuaNative *native, lua_State *state, int top) {
    native->settop(state, top);
}

FcLuaResult fc_lua_push(const FcLuaNative *native, lua_State *state, FC_LuaInput input) {
    PushOperation operation = {0};
    operation.operation.native = native;
    operation.input = input;
    return protect(state, &push_value, &operation.operation);
}

FcLuaResult fc_lua_read(const FcLuaNative *native, lua_State *state, int index, uint32_t kind, FC_Text tag, FC_LuaReadResult *out) {
    ReadOperation operation = {0};
    operation.operation.native = native;
    operation.index = index;
    operation.kind = kind;
    operation.tag = tag;
    operation.out = out;
    return protect(state, &read_value, &operation.operation);
}

FcLuaResult fc_lua_get(const FcLuaNative *native, lua_State *state, int table, int key) {
    TableOperation operation = {0};
    operation.operation.native = native;
    operation.table = table;
    operation.key = key;
    return protect(state, &access_table, &operation.operation);
}

FcLuaResult fc_lua_set(const FcLuaNative *native, lua_State *state, int table, int key, int value) {
    TableOperation operation = {0};
    operation.operation.native = native;
    operation.table = table;
    operation.key = key;
    operation.value = value;
    operation.write = 1;
    return protect(state, &access_table, &operation.operation);
}

FcLuaResult fc_lua_global_get(const FcLuaNative *native, lua_State *state, FC_Text name) {
    TableOperation operation = {0};
    operation.operation.native = native;
    operation.name = name;
    operation.global = 1;
    return protect(state, &access_table, &operation.operation);
}

FcLuaResult fc_lua_global_set(const FcLuaNative *native, lua_State *state, FC_Text name, int value) {
    TableOperation operation = {0};
    operation.operation.native = native;
    operation.name = name;
    operation.value = value;
    operation.global = 1;
    operation.write = 1;
    return protect(state, &access_table, &operation.operation);
}

FcLuaResult fc_lua_iteration_begin(const FcLuaNative *native, lua_State *state, int table, FC_LuaIterator *out) {
    IterationOperation operation = {0};
    operation.operation.native = native;
    operation.iterator = out;
    out->table = table;
    return protect(state, &iterate, &operation.operation);
}

FcLuaResult fc_lua_iteration_next(const FcLuaNative *native, lua_State *state, FC_LuaIterator *iterator, uint32_t *has_entry) {
    IterationOperation operation = {0};
    operation.operation.native = native;
    operation.iterator = iterator;
    operation.has_entry = has_entry;
    operation.action = 1;
    return protect(state, &iterate, &operation.operation);
}

FcLuaResult fc_lua_iteration_end(const FcLuaNative *native, lua_State *state, FC_LuaIterator *iterator) {
    IterationOperation operation = {0};
    operation.operation.native = native;
    operation.iterator = iterator;
    operation.action = 2;
    return protect(state, &iterate, &operation.operation);
}

FcLuaResult fc_lua_call(const FcLuaNative *native, lua_State *state, int function, int registry_reference, const FC_LuaValue *args,
                        uint32_t count, uint32_t forward_incoming) {
    CallOperation operation = {0};
    operation.operation.native = native;
    operation.function = function;
    operation.reference = registry_reference;
    operation.args = args;
    operation.count = count;
    operation.forward_incoming = forward_incoming;
    return protect(state, &call_function, &operation.operation);
}

FcLuaResult fc_lua_execute(const FcLuaNative *native, lua_State *state, FC_Text source) {
    CallOperation operation = {0};
    operation.operation.native = native;
    operation.source = source;
    operation.execute = 1;
    return protect(state, &call_function, &operation.operation);
}

FcLuaResult fc_lua_retain_registry(const FcLuaNative *native, lua_State *state, int function, int *out) {
    RegistryOperation operation = {0};
    operation.operation.native = native;
    operation.function = function;
    operation.out = out;
    return protect(state, &registry, &operation.operation);
}

FcLuaResult fc_lua_release_registry(const FcLuaNative *native, lua_State *state, int reference) {
    RegistryOperation operation = {0};
    operation.operation.native = native;
    operation.reference = reference;
    return protect(state, &registry, &operation.operation);
}

FcLuaResult fc_lua_copy_values(const FcLuaNative *native, lua_State *state, const FC_LuaValue *values, uint32_t count) {
    CopyOperation operation = {0};
    operation.operation.native = native;
    operation.values = values;
    operation.count = count;
    return protect(state, &copy_values, &operation.operation);
}

FcLuaResult fc_lua_copy_range(const FcLuaNative *native, lua_State *state, FC_LuaRange values) {
    CopyOperation operation = {0};
    operation.operation.native = native;
    operation.first = values.first;
    operation.count = values.count;
    return protect(state, &copy_values, &operation.operation);
}

FcLuaResult fc_lua_prepare_error(const FcLuaNative *native, lua_State *state, FC_Text error) {
    CopyOperation operation = {0};
    operation.operation.native = native;
    operation.error = error;
    operation.preparing_error = 1;
    return protect(state, &copy_values, &operation.operation);
}

FcLuaResult fc_lua_closure(const FcLuaNative *native, lua_State *state, FcLuaClosure *closure) {
    ClosureOperation operation = {0};
    operation.operation.native = native;
    operation.closure = closure;
    return protect(state, &closure_operation, &operation.operation);
}
