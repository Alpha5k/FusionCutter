#pragma once

#include <fc/detail/handler_adapter.hpp>
#include <fc/settings.hpp>
#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <typeindex>
#include <utility>
#include <vector>

// The SDK owns C++ plugin declarations and exposes stable C records through FC_GetPlugin.
// The runtime borrows those records while the plugin DLL remains loaded.
namespace fc {

// Selects the response to a recoverable patch failure during startup. Unexpected faults are always fatal.
enum class FailurePolicy : std::uint32_t {
    // Rejects the failed patch and permits independent patches to continue.
    Continue = FC_FAILURE_CONTINUE,
    // Stops the game.
    Fatal = FC_FAILURE_FATAL,
};

// Selects the game launch modes that may use a variant.
enum class Role : std::uint32_t {
    // Normal game launch, including a LAN or network game hosted by a player.
    Client = FC_ROLE_CLIENT,
    // Dedicated server launch.
    Server = FC_ROLE_SERVER,
    // Either launch mode.
    All = FC_ROLE_ALL,
};

// Names an image required by a variant.
struct ImageRequirement {
    // Variant-local label used when a handler selects this image in its plan.
    std::string name;
    // Exact profile IDs or prefixes ending in `*`; the runtime matches them against its image catalog.
    std::vector<std::string> profiles;
};

namespace detail {

class PluginStorage;

} // namespace detail

// Describes a handler implementation and its image, role, and dependency requirements.
// The handler's Settings type must match the effective schema when the plugin is exported.
class Variant {
  public:
    // Replaces the complete patch schema for this handler implementation.
    Variant &settings(SettingsSchema schema) {
        settings_ = std::move(schema);
        return *this;
    }

    // Overrides the patch's recoverable startup failure policy when this variant is selected; omission inherits it.
    Variant &failure_policy(FailurePolicy policy) {
        failure_policy_ = policy;
        return *this;
    }

    // Adds dependencies to those declared on the patch; controls determine enablement.
    Variant &depends_on(std::initializer_list<std::string_view> references) {
        for (const auto reference : references) {
            depends_on_.emplace_back(reference);
        }
        return *this;
    }

  private:
    template <class Handler> friend Variant variant(std::initializer_list<std::string_view>, Role);
    template <class Handler> friend Variant variant(std::initializer_list<ImageRequirement>, Role);
    friend class detail::PluginStorage;

    Variant(std::vector<ImageRequirement> images, Role role, decltype(FC_HandlerFactory::create) create)
        : images_(std::move(images)), role_(role), create_(create) {}

    std::vector<ImageRequirement> images_;
    Role role_ = Role::All;
    std::optional<FailurePolicy> failure_policy_;
    std::vector<std::string> depends_on_;
    // Points to create_handler in this plugin DLL.
    decltype(FC_HandlerFactory::create) create_{};
    // Absence inherits the patch schema; an engaged empty schema replaces it.
    std::optional<SettingsSchema> settings_;
    // PluginStorage checks this handler settings type against the effective schema before exporting it.
    std::type_index settings_type_{typeid(void)};
};

// Creates a variant with explicitly named required images; every requirement must match at runtime.
template <class Handler> Variant variant(std::initializer_list<ImageRequirement> images, Role role = Role::All) {
    Variant result{{images.begin(), images.end()}, role, &detail::create_handler<Handler>};
    if constexpr (requires { typename Handler::Settings; }) {
        result.settings_type_ = typeid(typename Handler::Settings);
    }
    return result;
}

// Creates one required game image whose profile selectors are alternatives.
template <class Handler> Variant variant(std::initializer_list<std::string_view> game_profiles, Role role = Role::All) {
    ImageRequirement game{.name = "game"};
    for (const auto profile : game_profiles) {
        game.profiles.emplace_back(profile);
    }
    return variant<Handler>({std::move(game)}, role);
}

// Declares common patch controls and settings, with variants selecting the handler for recognized images.
struct Patch {
    // This patch's configuration key within the plugin.
    std::string id;
    // Optional explanatory text.
    std::string description;
    // Optional Category::id.
    std::string category;

    // Required default startup state. Omitting it is an error.
    std::optional<bool> enabled;
    // Creates an individual configuration toggle. When false, a configurable group value takes
    // precedence, followed by enabled.
    bool configurable = true;

    // Default response to recoverable startup failure, inherited by variants without an override.
    FailurePolicy failure_policy = FailurePolicy::Continue;
    // Positive interval between handler updates.
    std::chrono::milliseconds update_interval{50};

    // Every variant inherits this complete schema unless it supplies its own.
    SettingsSchema settings;
    // These dependencies apply to every variant and constrain availability and order; controls determine enablement.
    std::vector<std::string> depends_on;
    // Ordered alternatives; runtime image and role selection determines which handler is constructed.
    std::vector<Variant> variants;
};

// Defines a configuration category shared by patches and groups.
struct Category {
    // Referenced by Patch::category and Group::category.
    std::string id;
    // Optional sort order; zero remains distinct from omission in the C ABI.
    std::optional<std::uint32_t> order;
};

// Declares a flat group of local patch IDs. A patch may belong to at most one group.
struct Group {
    std::string id;
    // Optional explanatory text.
    std::string description;
    // Optional Category::id.
    std::string category;
    // Each member is a local Patch::id from this plugin.
    std::vector<std::string> members;

    // Required group default.
    std::optional<bool> enabled;
    // When true, the group controls members whose configurable field is false.
    bool configurable = true;
};

// Contains one plugin declaration.
struct Plugin {
    // Compared ASCII case-insensitively while preserving this spelling for display and diagnostics.
    std::string id;
    // Distribution version shown in diagnostics; ABI compatibility is reported separately.
    std::string version;
    std::vector<Category> categories;
    std::vector<Group> groups;
    std::vector<Patch> patches;
};

} // namespace fc
