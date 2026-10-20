#pragma once

#include <fc/abi/common.h>
#include <fc/abi/native.h>

#ifdef __cplusplus
extern "C" {
#endif

// Appends status lines synchronously; the writer and context remain valid only during this status callback.
typedef struct FC_StatusWriter {
    void *context;
    // Returns 1 when the complete line fits the bounded status output, or 0 when it is rejected.
    uint32_t(FC_CDECL *line)(void *, FC_Text);
} FC_StatusWriter;

// Selects the active member of FC_Value.value.
enum FC_ScalarKind {
    FC_SCALAR_BOOL,
    FC_SCALAR_SIGNED,
    FC_SCALAR_UNSIGNED,
    FC_SCALAR_REAL,
    FC_SCALAR_TEXT
};

// Carries one typed scalar across the C ABI.
typedef struct FC_Value {
    // One FC_ScalarKind value selects the corresponding union member.
    uint32_t kind;

    union {
        // Boolean scalars use 0 or 1.
        uint32_t boolean;
        int64_t signed_value;
        uint64_t unsigned_value;
        double real;
        // Storage lasts for the enclosing call or immutable declaration; scalar text may contain null bytes.
        FC_Text text;
    } value;
} FC_Value;

// Declares one scalar setting and its plugin-owned validator; strings and context outlive declaration use.
typedef struct FC_Setting {
    // key names the INI key; section is an optional patch subsection; environment optionally names an override variable.
    FC_Text key, description, section, environment;
    FC_Value default_value;
    // Author-facing constraint text emitted in generated configuration comments.
    FC_Text constraints;
    void *context;
    // Checks conversion, narrowing and author constraints without retaining the input.
    // Returns exactly 1 on acceptance, or 0 with an optional synchronous error.
    uint32_t(FC_CDECL *validate)(void *, const FC_Value *, FC_ErrorSink);
} FC_Setting;

// Lifecycle callbacks run in the allocating module; none may throw through this C boundary.
// plan and prepare return 1 on success or 0 with an optional synchronous error; optional lifecycle callbacks are null.
typedef struct FC_Handler {
    // Opaque object allocated by the plugin DLL.
    void *context;
    // Frees context in the plugin DLL that allocated it.
    void(FC_CDECL *destroy)(void *context);
    // Declares native changes before validation and preparation; the plan is valid only during this call.
    uint32_t(FC_CDECL *plan)(void *, FC_Plan *, FC_ErrorSink);
    // Initializes handler state after native resources are prepared and before native changes are installed.
    uint32_t(FC_CDECL *prepare)(void *, FC_ErrorSink);
    // Runs after native installation, before the runtime publishes the patch as installed.
    void(FC_CDECL *activate)(void *);
    // Updates installed handler state; now_ns is steady-clock time since its epoch, in nanoseconds.
    // elapsed_ns is nanoseconds since activation or the previous update.
    void(FC_CDECL *update)(void *, int64_t now_ns, int64_t elapsed_ns);
    // Writes the installed handler's status using the callback-scoped writer.
    void(FC_CDECL *status)(void *, FC_StatusWriter *);
} FC_Handler;

// Creates handlers in the plugin DLL from schema-ordered values whose storage lasts only for the call.
// owner and host provide stable runtime access for the constructed handler.
typedef struct FC_HandlerFactory {
    // State kept valid while the runtime uses this factory.
    void *context;
    // Returns 1 with context, destroy and implemented callbacks set, or 0 with an optional error.
    // out must point to writable caller storage; failure clears it before invoking the optional error callback.
    uint32_t(FC_CDECL *create)(void *context, void *owner, const FC_Host *host, const FC_Value *values, uint32_t count, FC_Handler *out,
                               FC_ErrorSink error);
} FC_HandlerFactory;

// Selects the game launch modes that may use a variant.
enum FC_Role {
    // Normal game launch, including a LAN or network game hosted by a player.
    FC_ROLE_CLIENT = 1,
    // Dedicated server launch.
    FC_ROLE_SERVER = 2,
    // Either launch mode.
    FC_ROLE_ALL = 3
};

// Selects the response to a recoverable patch failure during startup. Unexpected faults are always fatal.
enum FC_FailurePolicy {
    // Rejects the failed patch and permits independent patches to continue.
    FC_FAILURE_CONTINUE = 0,
    // Stops the game.
    FC_FAILURE_FATAL = 1
};

// Defines a configuration category shared by patches and groups.
typedef struct FC_Category {
    // Referenced by FC_Patch.category and FC_Group.category.
    FC_Text id;
    // 1 when order is present, including explicit zero; 0 when order is omitted.
    uint32_t has_order;
    uint32_t order;
} FC_Category;

// Defines a flat group of patches in one plugin.
typedef struct FC_Group {
    // Local group ID used in configuration and dependencies; it cannot contain '.'.
    FC_Text id;
    FC_Text description;
    // Optional FC_Category.id controlling where the group's toggle is generated.
    FC_Text category;
    // member_count local patch IDs owned by the plugin; qualified dependency references are invalid.
    const FC_Text *members;
    uint32_t member_count;
    // Required group default; use 0 or 1.
    uint32_t enabled;
    // When 1, the group controls members whose configurable field is 0; use 0 or 1.
    uint32_t configurable;
} FC_Group;

// Names an image required by a variant.
typedef struct FC_ImageRequirement {
    // Variant-local label used to select this image in the handler's plan.
    FC_Text name;
    // selector_count selectors stored by the plugin: exact profile IDs or prefixes ending in '*'.
    // The runtime matches them against its image catalog.
    const FC_Text *selectors;
    uint32_t selector_count;
} FC_ImageRequirement;

// Handler implementation choice for a patch.
typedef struct FC_Variant {
    // images points to image_count records owned by the plugin.
    const FC_ImageRequirement *images;
    uint32_t image_count;
    // One FC_Role value determines which launch modes may select this variant.
    uint32_t role;
    // Effective FC_FailurePolicy value, already resolved by the SDK from the patch default and optional variant override.
    uint32_t failure_policy;
    // depends_on points to dependency_count records stored by the plugin. These dependencies supplement the
    // patch dependencies and constrain availability and order; controls determine enablement.
    const FC_Text *depends_on;
    uint32_t dependency_count;
    // Effective scalar schema, already resolved by the SDK from the patch and variant declarations.
    const FC_Setting *settings;
    uint32_t setting_count;
    // factory creates the handler and supplies the callback that destroys it in the plugin DLL.
    FC_HandlerFactory factory;
} FC_Variant;

// Declares a patch's controls, common dependencies, and alternative handler variants.
typedef struct FC_Patch {
    // Local ID used in configuration and qualified dependencies; it cannot contain '.'.
    FC_Text id;
    FC_Text description;
    // Optional FC_Category.id controlling where the patch's toggle is generated.
    FC_Text category;
    // Required patch default; use 0 or 1.
    uint32_t enabled;
    // Use 0 or 1. When 1, the patch has its own toggle. Otherwise a configurable group takes precedence
    // over the enabled value.
    uint32_t configurable;
    // One FC_FailurePolicy value supplies the variant default and governs recoverable startup failures before Ready selection.
    uint32_t failure_policy;
    // update_interval_ms must be positive.
    int64_t update_interval_ms;
    // depends_on points to dependency_count records stored by the plugin. Dependencies constrain availability
    // and order; controls determine enablement.
    const FC_Text *depends_on;
    uint32_t dependency_count;
    // variants points to variant_count records owned by the plugin.
    const FC_Variant *variants;
    uint32_t variant_count;
} FC_Patch;

// Plugin declaration returned by FC_GetPlugin; its nested arrays and views remain immutable during runtime use.
// Array pointers may be null only when their counts are zero.
typedef struct FC_Plugin {
    // Number of supplied bytes, allowing later ABI versions to append fields.
    uint32_t byte_size;
    // ID text stored by the plugin, valid while the DLL is loaded.
    FC_Text id;
    // Version text stored by the plugin, valid while the DLL is loaded.
    FC_Text version;
    // category_count records and nested text views stored by the plugin.
    const FC_Category *categories;
    uint32_t category_count;
    // group_count records and nested views stored by the plugin.
    const FC_Group *groups;
    uint32_t group_count;
    // patch_count records and nested views stored by the plugin.
    const FC_Patch *patches;
    uint32_t patch_count;
} FC_Plugin;

// With FC_ABI_VERSION and writable out, FC_GetPlugin returns 1 with a declaration valid while the DLL is loaded.
// On failure it returns 0, clears a supplied out, and reports through error when possible.
typedef uint32_t(FC_CDECL *FC_GetPluginFn)(uint32_t abi, const FC_Plugin **out, FC_ErrorSink error);

#ifdef __cplusplus
}
#endif
