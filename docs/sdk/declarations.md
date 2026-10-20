# Plugins, patches and variants

Include `<fc/plugin.hpp>`. Return an `fc::Plugin` from a factory and export it with `FC_PLUGIN(factory)`.
Declarations describe the available work; handlers perform it after selection.

## Describe a feature once, implement it for the builds it supports

Declarations let FC make decisions before it runs your patch code. The runtime needs to know which game
builds you support, which settings to read, whether the player enabled the feature and whether another patch
must install first. Keeping that information in a declaration also gives configuration and diagnostics the
same names and defaults.

Use a patch for a feature with its own startup state and failure outcome. Use a variant when the same feature
needs a different implementation or layout on another build. For example, a chat display feature can remain
one patch even if Steam and GOG use different game addresses. A separate diagnostics feature usually belongs
in its own patch so it can have its own controls and dependencies.

The plugin ID, patch ID and profile ID answer different questions:

| Example | Identifies |
| --- | --- |
| `Example` | The plugin, including its `FC.Example.ini` configuration file |
| `Example.ChatDisplay` | One patch, used in dependencies and reports |
| `BF2.Steam.59EDE353` | A recognized game image build that a variant can select |

Renaming a patch or plugin changes the identity used by configuration and consumers. A new displayed version
does not change that identity. Choose names that describe the feature rather than a temporary implementation.

## Plugin and patch fields

| `Plugin` field | Meaning |
| --- | --- |
| `id` | Plugin identity; used in diagnostics, qualified dependencies and `FC.<id>.ini` |
| `version` | Distribution version displayed in reports; separate from ABI compatibility |
| `categories` | Configuration categories referenced by patches and groups |
| `groups` | Flat groups of patches in this plugin |
| `patches` | Patch declarations |

Plugin, patch, group, category and image-label lookups are ASCII case-insensitive. Keep their declared
spelling consistent for reports. Plugin, patch and group IDs cannot contain `.` because qualified
dependencies use that separator. Patch and group IDs share a namespace within a plugin. IDs and settings
must also be valid for their filenames or INI positions; use simple names such as `Example`, `FrameRate`
and `Network`.

| `Patch` field | Default and meaning |
| --- | --- |
| `id` | Required local patch identity |
| `description` | Optional explanation used in configuration |
| `category` | Optional declared category ID |
| `enabled` | Required boolean default; omission is an export error |
| `configurable` | `true`; creates a patch toggle |
| `failure_policy` | `FailurePolicy::Continue`; inherited by variants unless replaced |
| `update_interval` | 50 milliseconds; must be positive |
| `settings` | Common typed schema, empty by default |
| `depends_on` | Required patch or group references |
| `variants` | Alternative handler implementations and their image requirements |

## Variants and image selection

`variant<Handler>` accepts either a list of alternative profiles for the `game` image, or a list of named
image requirements. All named requirements must match; profiles within one requirement are alternatives.

Both overloads return Variant. The full form takes `ImageRequirement` records with two owning fields:
`name` is the variant-local label and `profiles` is its list of selectors. The short form constructs one
requirement named `game`. Every requirement needs a label and at least one profile selector.

The distinction matters when one patch touches several modules. Supporting either Steam or GOG means one
game requirement with two alternatives. Modifying the game and GalaxyPeer means two requirements, each with
its own alternatives. The runtime waits for a missing supporting DLL instead of running the handler against
an incomplete set of images.

These declarations are equivalent:

```cpp
fc::variant<Handler>({"BF2.Steam.59EDE353", "BF2.GOG.59EDF52B"});
fc::variant<Handler>({{"game", {"BF2.Steam.59EDE353", "BF2.GOG.59EDF52B"}}});
```

A variant needing both the game and GalaxyPeer uses the full form:

```cpp
fc::variant<Handler>({
    {"game", {"BF2.Steam.59EDE353", "BF2.GOG.59EDF52B"}},
    {"peer", {"GalaxyPeer.59E6304A", "GalaxyPeer.5BBE22A6"}}
});
```

Within `plan`, select a label with `plan.for_image("game")` or `plan.for_image("peer")`. That view's
`profile()` returns the selected full profile ID so the handler can choose the corresponding layout.
One selected image binds automatically; multiple images require an explicit view for bare RVAs.

The label `peer` is your name for the DLL's role in this patch. It is not the DLL filename or its build ID.
That lets the handler request the same image view while using `profile()` to choose the selected build's
layout. Put address differences in that layout; keep shared behavior in the handler.

A selector ending in `*` matches recognized profile IDs with that prefix, for example `BF2.Steam.*`.
It does not recognize new executables by itself. The runtime catalog must recognize the image, and your
handler must support its layout. See the [profile table](../installation.md#recognized-builds).

Variants must not overlap for the same loaded images and role. Their order does not choose a winner.
A missing required DLL can leave a variant waiting; an incompatible executable cannot arrive later.

## Launch roles and variant overrides

The optional second argument is `Role::Client`, `Role::Server` or `Role::All`. Omitting it selects All.
Client includes a player hosting a session. Server means the dedicated loader's launch role, independently
of the current [game mode](game.md).

```cpp
fc::Patch{
    .id = "Transport",
    .enabled = true,
    .failure_policy = fc::FailurePolicy::Continue,
    .variants = {
        fc::variant<ClientHandler>({"BF2.GOG.59EDF52B"}, fc::Role::Client),
        fc::variant<ServerHandler>({"BF2.GOG.59EDF52B"}, fc::Role::Server)
            .failure_policy(fc::FailurePolicy::Fatal)
    }
};
```

Variant modifiers return the same declaration for chaining:

| Method | Effect |
| --- | --- |
| `.settings(schema)` | Replaces the entire patch schema, including with an empty schema |
| `.depends_on({references})` | Adds to the patch's dependencies |
| `.failure_policy(policy)` | Replaces the patch default for this variant |

`Continue` rejects a recoverable failed patch while independent patches continue. `Fatal` stops the game.
The policy does not make arbitrary faults in game or plugin code recoverable. See [lifecycle](lifecycle.md).

Use the variant's role to decide whether the patch should install at all for a client or dedicated launch.
Use game context later when behavior depends on whether a player is currently hosting, joining or playing
locally. A player can change sessions without restarting FC or selecting a new variant.

Failure policy expresses whether the rest of this game process can usefully continue if startup rejects
the patch. A cosmetic patch can usually use Continue. A server feature required for the server's intended
operation may use Fatal on its server variant. This choice concerns failure to install; it does not replace
normal handling of a refused message or another expected error after startup.

## Dependencies

An unqualified reference such as `Provider` selects a patch or group in the same plugin. `Example.Provider`
selects one in another plugin. Use `.` as the separator:

```cpp
.depends_on = {"core.lua_bindings", "Provider"}
```

A dependency must be enabled, compatible and successfully installed. Dependencies determine ordering and
availability; they never turn patches on. A group dependency requires every member. Missing references,
cycles and failed providers prevent dependent patches from running.

Use a dependency when the patch cannot operate without the provider. For optional API integration, use
[`PrepareContext::on_available`](patch_apis.md) without a required dependency.

For example, a patch exposing Lua functions depends on `core.lua_bindings` because the provider must be ready
to accept its registrations. The dependency supplies ordering and availability. The Lua helper supplies the
actual operations. Merely naming a dependency does not retrieve an API or call the provider.

## Categories and groups

Define categories once and reference them from patches or groups. `Category` has `id` and optional `order`.
An explicitly supplied zero is a real order value. Categories control configuration presentation.

A category answers where a player finds controls. A group answers which patches share a control or must all
be available to a dependent patch. Several unrelated client features can share a `Client` category without
forming a group or depending on one another.

```cpp
.categories = {{.id = "Client", .order = 100}, {.id = "Network", .order = 200}}
```

`Group` contains `id`, optional `description` and `category`, a `members` list, required `enabled`, and
`configurable` defaulting to true. Members are local patch IDs. A patch can belong to at most one group;
groups cannot contain other groups.

```cpp
.groups = {{.id = "NetworkFeatures",
            .category = "Network",
            .members = {"Transport", "PeerStatus"},
            .enabled = true}}
```

For group control, declare member patches with `.configurable = false`. An individually configurable patch
uses its own configured value or default and ignores the group toggle. Otherwise a configurable group supplies
its configured value or default; without one, the patch uses its own `enabled` default.

A non-configurable patch can still expose scalar settings. `configurable` controls the enabled toggle,
not its entire settings schema. The generated INI rules are in [configuration](../configuration.md).

For example, a configurable `NetworkFeatures` group can control non-configurable `Transport` and `PeerStatus`
members. If `PeerStatus` instead has its own configurable toggle, that toggle controls it independently of
the group. A dependency on `NetworkFeatures` still requires both members; the independent toggle can therefore
make that dependency unavailable. Group control and group dependencies are separate uses of the same membership.
