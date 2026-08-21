#include "catalog_builder.hpp"
#include "callback_error.hpp"
#include "definition_copy.hpp"
#include "recognition.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

// The native fixtures use one inert lifecycle so catalog tests vary registration metadata without runtime behavior.
[[nodiscard]] constexpr FC_StringView text(std::string_view value) noexcept {
    return {value.data(), static_cast<std::uint32_t>(value.size())};
}

FC_CallStatus FC_CALL create_patch(void*, const FC_CreateContext*, const FC_SettingsView*, const FC_ErrorSink*,
                                   FC_PatchHandle* output) {
    *output = reinterpret_cast<FC_PatchHandle>(std::uintptr_t{1});
    return FC_CALL_OK;
}

void FC_CALL destroy_patch(void*, FC_PatchHandle) {}

FC_CallStatus FC_CALL plan_patch(void*, FC_PatchHandle, const FC_PlanContext*, const FC_PlanSink*,
                                 const FC_ErrorSink*) {
    return FC_CALL_OK;
}

// Owns every string and child array behind a mutable native definition used to probe admission boundaries.
class NativePlugin {
  public:
    NativePlugin(std::string plugin_id, std::size_t group_count = 0, bool with_patch = true)
        : plugin_id_(std::move(plugin_id)) {
        patch_id_ = plugin_id_ + "Patch";
        group_ids_.reserve(group_count);
        groups_.resize(group_count);
        for (std::size_t index = 0; index < group_count; ++index) {
            group_ids_.push_back("Group" + std::to_string(index));
            groups_[index].id = text(group_ids_[index]);
        }
        supports_[0] = {.layouts = FC_LAYOUT_GAMESPY_RETAIL,
                        .roles = FC_HOST_ROLE_CLIENT,
                        .image = FC_IMAGE_GAME,
                        .callbacks = {.create = &create_patch, .destroy = &destroy_patch, .plan = &plan_patch},
                        .failure_policy = FC_FAILURE_INHERIT};
        patch_ = {.id = text(patch_id_),
                  .name = text(patch_name_),
                  .configurable = FC_TRUE,
                  .enabled = FC_FALSE,
                  .failure_policy = FC_FAILURE_CONTINUE,
                  .supports = supports_.data(),
                  .support_count = 1};
        with_patch_ = with_patch;
        refresh();
    }

    NativePlugin(const NativePlugin&) = delete;
    NativePlugin& operator=(const NativePlugin&) = delete;

    void set_source(std::string value) {
        source_ = std::move(value);
        refresh();
    }

    void set_patch_id(std::string value) {
        patch_id_ = std::move(value);
        patch_.id = text(patch_id_);
        refresh();
    }

    void add_steam_support() noexcept {
        // The second support is the only one matching the synthetic Steam target, making its ordinal observable.
        supports_[1] = supports_[0];
        supports_[1].layouts = FC_LAYOUT_STEAM_RETAIL;
        patch_.support_count = 2;
    }

    // Selects the late tuple from ABI generation 1 and lets tests vary direct or inherited fatal failure policy.
    void use_fatal_late_support(bool inherited) noexcept {
        supports_[0].layouts = FC_LAYOUT_GOG_RETAIL;
        supports_[0].roles = FC_HOST_ROLE_SERVER;
        supports_[0].image = FC_IMAGE_GALAXY_PEER;
        supports_[0].failure_policy = inherited ? FC_FAILURE_INHERIT : FC_FAILURE_FATAL;
        patch_.failure_policy = inherited ? FC_FAILURE_FATAL : FC_FAILURE_CONTINUE;
    }

    void refresh() noexcept {
        // Rebind every borrowed view after owned fixture storage changes, matching a plugin's synchronous ABI tree.
        native_ = {.struct_size = sizeof(FC_PluginDefinition),
                   .id = text(plugin_id_),
                   .source = text(source_),
                   .groups = groups_.data(),
                   .group_count = static_cast<std::uint32_t>(groups_.size()),
                   .patches = with_patch_ ? &patch_ : nullptr,
                   .patch_count = with_patch_ ? 1U : 0U};
    }

    [[nodiscard]] const FC_PluginDefinition* native() const noexcept {
        return &native_;
    }

    [[nodiscard]] std::size_t copied_fixed_size() const noexcept {
        // Excludes the variable source string so tests can place that one field exactly at the metadata budget edge.
        return sizeof(FC_PluginDefinition) + sizeof(FC_PatchDefinition) + sizeof(FC_SupportDefinition) +
               plugin_id_.size() + patch_id_.size() + patch_name_.size();
    }

  private:
    std::string plugin_id_;
    std::string source_;
    std::string patch_id_;
    std::string patch_name_{"Fixture patch"};
    std::vector<std::string> group_ids_;
    std::vector<FC_GroupDefinition> groups_;
    std::array<FC_SupportDefinition, 2> supports_{};
    FC_PatchDefinition patch_{};
    bool with_patch_{};
    FC_PluginDefinition native_{};
};

// Fixed registration slots let each template callback retain C linkage while selecting independent fixture state.
std::array<const FC_PluginDefinition*, 8> g_definitions{};

template <std::size_t Slot>
FC_CallStatus FC_CALL register_definition(const FC_HostApi*, const FC_RegistrySink* registry, const FC_ErrorSink*) {
    return registry->submit(registry->context, g_definitions[Slot]) == FC_SUBMIT_ACCEPTED ? FC_CALL_OK : FC_CALL_FAILED;
}

void release_definition() noexcept {}

template <std::size_t Slot> [[nodiscard]] fc::catalog::RegistrationBridge bridge() noexcept {
    return {&register_definition<Slot>, &release_definition};
}

// Common owners and target facts keep every CatalogBuilder case focused on the admission condition it varies.
[[nodiscard]] const FC_HostApi& host() noexcept {
    static const FC_HostApi value{.struct_size = sizeof(FC_HostApi)};
    return value;
}

[[nodiscard]] fc::catalog::CodeOwner code_owner() {
    auto owner = fc::catalog::CodeOwner::from_address(reinterpret_cast<std::uintptr_t>(&create_patch));
    REQUIRE(owner.has_value());
    return std::move(*owner);
}

[[nodiscard]] fc::targets::RecognizedTarget target() {
    std::vector<std::byte> bytes(1);
    std::vector<fc::targets::OwnedImage> images;
    images.push_back(fc::targets::OwnedImage::mapped({FC_IMAGE_GAME, "SteamRetail_Game_59EDE353", 0, bytes.size()},
                                                     std::move(bytes), {}));
    auto recognized = fc::targets::RecognizedTarget::create(FC_HOST_ROLE_CLIENT, std::move(images));
    REQUIRE(recognized.has_value());
    return std::move(*recognized);
}

[[nodiscard]] fc::catalog::CatalogBuildResult build(fc::catalog::CatalogBuilder& builder) {
    // Every build receives an isolated installation tree so generation and discovery cannot leak between cases.
    static std::atomic_uint32_t sequence;
    const auto root = std::filesystem::temp_directory_path() /
                      ("FusionCutter-catalog-" + std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)));
    std::filesystem::create_directories(root / "plugins");
    auto current_target = target();
    auto result = builder.build(current_target, fc::config::ConfigurationPaths{root});
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    return result;
}

[[nodiscard]] fc::catalog::PluginDefinitionRecord valid_record() {
    NativePlugin plugin{"ValidationProbe"};
    auto copied = fc::catalog::copy_plugin_definition(plugin.native());
    REQUIRE(copied.has_value());
    return std::move(*copied);
}

} // namespace

TEST_CASE("copied metadata accepts its exact byte capacity and rejects the next byte") {
    // Vary only the source string so fixed record and identity costs remain constant across the boundary pair.
    NativePlugin plugin{"BudgetProbe"};
    REQUIRE(plugin.copied_fixed_size() < fc::catalog::kPluginMetadataByteCapacity);
    plugin.set_source(std::string(fc::catalog::kPluginMetadataByteCapacity - plugin.copied_fixed_size(), 'x'));
    CHECK(fc::catalog::copy_plugin_definition(plugin.native()).has_value());

    plugin.set_source(std::string(fc::catalog::kPluginMetadataByteCapacity - plugin.copied_fixed_size() + 1, 'x'));
    CHECK_FALSE(fc::catalog::copy_plugin_definition(plugin.native()).has_value());
}

TEST_CASE("callback errors share the exact UTF-8 byte budget and truncate only the excess") {
    const std::string message(fc::catalog::kCallbackErrorByteCapacity - 1, 'm');
    constexpr std::string_view operation = "op";
    fc::catalog::CallbackError error;
    const auto sink = error.sink();

    // The message leaves one byte for the operation; the following byte is the first data outside fixed storage.
    sink.set(sink.context, text(message), text(operation));
    CHECK(error.message == message);
    CHECK(error.operation == "o");
    CHECK(error.message.size() + error.operation.size() == fc::catalog::kCallbackErrorByteCapacity);
}

TEST_CASE("late supports reject direct and inherited fatal failure policies") {
    const auto owner = code_owner();
    for (const bool inherited : {false, true}) {
        NativePlugin native{"LateFatalProbe"};
        native.use_fatal_late_support(inherited);
        auto copied = fc::catalog::copy_plugin_definition(native.native());
        REQUIRE(copied.has_value());
        INFO("Inherited fatal failure policy: " << inherited);
        CHECK_FALSE(
            fc::catalog::validate_plugin_definition(*copied, fc::catalog::PluginOrigin::Bundled, owner).has_value());
    }
}

TEST_CASE("structural admission rejects each target-independent definition family") {
    const auto owner = code_owner();
    CHECK(
        fc::catalog::validate_plugin_definition(valid_record(), fc::catalog::PluginOrigin::Bundled, owner).has_value());
    auto noncanonical_core = valid_record();
    noncanonical_core.id = "core";
    CHECK_FALSE(
        fc::catalog::validate_plugin_definition(noncanonical_core, fc::catalog::PluginOrigin::Core, owner).has_value());

    // Each row mutates exactly one independently owned rule family while retaining an otherwise valid definition.
    for (std::size_t row = 0; row < 10; ++row) {
        auto plugin = valid_record();
        switch (row) {
        case 0:
            plugin.id = "1Invalid";
            break;
        case 1:
            plugin.categories.push_back({.id = "General"});
            break;
        case 2:
            plugin.groups.push_back({.id = plugin.patches.front().id});
            break;
        case 3:
            plugin.patches.front().supports.front().layouts = FC_LAYOUT_MOD_TOOLS;
            plugin.patches.front().supports.front().roles = FC_HOST_ROLE_SERVER;
            break;
        case 4:
            plugin.patches.front().supports.push_back(plugin.patches.front().supports.front());
            break;
        case 5:
            plugin.patches.front().supports.front().callbacks.create = nullptr;
            break;
        case 6: {
            fc::catalog::SettingDefinitionRecord setting;
            setting.key = "Value";
            setting.type = FC_SETTING_STRING;
            setting.has_range = FC_TRUE;
            setting.default_string = "valid";
            plugin.patches.front().settings.push_back(std::move(setting));
            break;
        }
        case 7:
            plugin.groups.push_back({.id = "FixtureGroup", .members = {"MissingPatch"}});
            break;
        case 8:
            plugin.patches.front().category = "MissingCategory";
            break;
        case 9:
            plugin.id = "fusioncutter";
            break;
        default:
            FAIL("Unexpected structural validation row");
        }
        INFO("Structural validation row: " << row);
        CHECK_FALSE(
            fc::catalog::validate_plugin_definition(plugin, fc::catalog::PluginOrigin::Bundled, owner).has_value());
    }
}

TEST_CASE("collision participation is simultaneous and does not backtrack") {
    // Two collisions between plugin IDs and one collision between patch IDs form a single frozen loser set.
    NativePlugin first{"SharedPlugin"};
    NativePlugin second{"SharedPlugin"};
    NativePlugin third{"ThirdPlugin"};
    third.set_patch_id("SharedPluginPatch");
    g_definitions[0] = first.native();
    g_definitions[1] = second.native();
    g_definitions[2] = third.native();

    fc::catalog::CatalogBuilder builder{host(), code_owner()};
    builder.add_core(fc::catalog::core_registration_bridge());
    builder.add_bundled(bridge<0>());
    builder.add_bundled(bridge<1>());
    builder.add_bundled(bridge<2>());
    auto result = build(builder);

    // Only the built-in Core plugin survives; removing either loser must not rescue the third participant.
    REQUIRE(result.catalog.has_value());
    CHECK(result.catalog->plugins().size() == 1);
    CHECK(result.rejections.size() == 3);
    CHECK(std::ranges::all_of(result.rejections, [](const auto& rejection) {
        return rejection.stage == fc::catalog::AdmissionStage::Collision && rejection.plugin_id.has_value() &&
               !rejection.reason.empty();
    }));
}

TEST_CASE("global definition capacity follows the built-in Core plugin then the sorted order of bundled plugins") {
    // The built-in Core plugin leaves one definition slot, so the order of bundled plugins selects the sole survivor.
    NativePlugin core{"Core", 4095, false};
    NativePlugin zulu{"ZuluPlugin"};
    NativePlugin alpha{"AlphaPlugin"};
    g_definitions[0] = core.native();
    g_definitions[1] = zulu.native();
    g_definitions[2] = alpha.native();

    fc::catalog::CatalogBuilder builder{host(), code_owner()};
    builder.add_core(bridge<0>());
    builder.add_bundled(bridge<1>());
    builder.add_bundled(bridge<2>());
    auto result = build(builder);

    REQUIRE(result.catalog.has_value());
    REQUIRE(result.catalog->plugins().size() == 2);
    CHECK(result.catalog->plugins()[0].definition.id == "Core");
    CHECK(result.catalog->plugins()[1].definition.id == "AlphaPlugin");
    REQUIRE(result.rejections.size() == 1);
    CHECK(result.rejections.front().plugin_id == "ZuluPlugin");
    CHECK(result.rejections.front().stage == fc::catalog::AdmissionStage::Capacity);
    CHECK_FALSE(result.rejections.front().reason.empty());
}

TEST_CASE("built-in Core plugin overflow is fatal") {
    NativePlugin core{"Core", 4097, false};
    g_definitions[0] = core.native();

    fc::catalog::CatalogBuilder builder{host(), code_owner()};
    builder.add_core(bridge<0>());
    auto result = build(builder);

    CHECK_FALSE(result.catalog.has_value());
    CHECK(result.fatal_error.has_value());
    CHECK_FALSE(result.fatal_error->empty());
}

TEST_CASE("the final plugin catalog retains the selected support ordinal and aligned configuration snapshot") {
    NativePlugin plugin{"SelectedProbe", 1};
    plugin.add_steam_support();
    g_definitions[0] = plugin.native();

    fc::catalog::CatalogBuilder builder{host(), code_owner()};
    builder.add_core(fc::catalog::core_registration_bridge());
    builder.add_bundled(bridge<0>());
    auto result = build(builder);

    // Indices in the plugin catalog and configuration must describe the same admitted plugin, patch, and group order.
    REQUIRE(result.catalog.has_value());
    REQUIRE(result.configuration.has_value());
    REQUIRE(result.catalog->plugins().size() == 2);
    REQUIRE(result.configuration->plugins.size() == 2);
    const auto& final_patch = result.catalog->plugins()[1].definition.patches.front();
    REQUIRE(final_patch.selected_support.has_value());
    CHECK(*final_patch.selected_support == 1);
    CHECK(result.catalog->find_patch("selectedprobepatch").has_value());
    CHECK(result.catalog->find_plugin("selectedprobe").has_value());
    CHECK(result.catalog->find_group("group0").has_value());
    CHECK(result.configuration->plugins[1].plugin_id == "SelectedProbe");
}

TEST_CASE("malformed built-in Core plugin configuration is fatal before transfer to the plugin catalog") {
    static std::atomic_uint32_t sequence;
    const auto root = std::filesystem::temp_directory_path() /
                      ("FusionCutter-core-config-" + std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)));
    std::filesystem::create_directories(root / "plugins");
    std::filesystem::create_directories(root / "config");
    {
        // A lone carriage return is a whole-file error, not a recoverable unknown setting diagnostic.
        std::ofstream output{root / "config" / "FC.Core.ini", std::ios::binary};
        REQUIRE(output.is_open());
        output << "[FusionCutter]\rLogLevel=Debug\n";
        REQUIRE(output.good());
    }

    fc::catalog::CatalogBuilder builder{host(), code_owner()};
    builder.add_core(fc::catalog::core_registration_bridge());
    auto current_target = target();
    bool configuration_started{};
    // The observer marks the internal phase before the malformed file fails, preserving accurate crash attribution.
    const fc::catalog::CatalogBuildObserver observer{.context = &configuration_started,
                                                     .begin_configuration = [](void* context) noexcept {
                                                         *static_cast<bool*>(context) = true;
                                                     }};
    auto result = builder.build(current_target, fc::config::ConfigurationPaths{root}, observer);
    CHECK(configuration_started);
    CHECK_FALSE(result.catalog.has_value());
    CHECK_FALSE(result.configuration.has_value());
    REQUIRE(result.fatal_error.has_value());
    CHECK_FALSE(result.fatal_error->empty());

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}
