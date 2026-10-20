#include "plugins/plugin.hpp"
#include "../test_support.hpp"

#include <fc/plugin.hpp>

#include <catch2/catch_test_macros.hpp>

#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace {

// PluginStorage must keep the same address because its exported C records point into the object.
static_assert(!std::is_copy_constructible_v<fc::detail::PluginStorage> && !std::is_copy_assignable_v<fc::detail::PluginStorage>);
static_assert(!std::is_move_constructible_v<fc::detail::PluginStorage> && !std::is_move_assignable_v<fc::detail::PluginStorage>);

// Matches the FC_GetPlugin signature for lookup through GetProcAddress.
using GetPluginFunction = std::uint32_t(__cdecl *)(std::uint32_t, const FC_Plugin **, FC_ErrorSink);

// Stores errors reported through FC_ErrorSink for later assertions.
struct CallbackErrorCapture {
    std::string message;

    // Copies the callback's borrowed message before it expires.
    static void __cdecl write(void *context, FC_Text message, std::uint32_t) {
        auto &self = *static_cast<CallbackErrorCapture *>(context);
        self.message.assign(message.data, message.size);
    }

    // Returns a sink that writes into this capture, which must outlive every callback using it.
    FC_ErrorSink sink() {
        return {.context = this, .write = &write};
    }
};

// Owns a LoadLibrary reference while a test calls exported functions.
class PluginModule {
  public:
    // Loads the DLL and owns its loader reference; a failed load leaves the module empty.
    explicit PluginModule(const std::filesystem::path &path) : value_(LoadLibraryW(path.c_str())) {}

    // Releases this loader reference after the test finishes using the module's exports.
    ~PluginModule() {
        if (value_ != nullptr) {
            FreeLibrary(value_);
        }
    }

    PluginModule(const PluginModule &) = delete;
    PluginModule &operator=(const PluginModule &) = delete;

    explicit operator bool() const {
        return value_ != nullptr;
    }

    // Looks up an export, returning null if absent; a returned address is usable only while the DLL stays loaded.
    FARPROC symbol(const char *name) const {
        return GetProcAddress(value_, name);
    }

  private:
    HMODULE value_{};
};

// Returns the fixture declaration, keeping its DLL loaded for the lifetime of the tests.
const FC_Plugin *fixture_descriptor() {
    // Copies of the declaration still refer to strings and nested arrays in this DLL.
    static PluginModule module{FC_FIXTURE_PLUGIN};
    REQUIRE(module);
    const auto entry = reinterpret_cast<GetPluginFunction>(module.symbol("FC_GetPlugin"));
    REQUIRE(entry != nullptr);

    const FC_Plugin *descriptor{};
    CallbackErrorCapture error;
    REQUIRE(entry(FC_ABI_VERSION, &descriptor, error.sink()) == 1);
    REQUIRE(descriptor != nullptr);
    return descriptor;
}

// Returns whether any admission diagnostic includes the requested message fragment.
bool has_diagnostic_containing(const fc::runtime::AdmissionResult &result, std::string_view part) {
    for (const auto &diagnostic : result.diagnostics) {
        if (diagnostic.message.find(part) != std::string::npos) {
            return true;
        }
    }

    return false;
}

// Copies the FC_Plugin record and nested arrays, then updates their pointers.
// Tests mutate the copy while the fixture declaration remains stable.
struct MutablePluginGraph {
    FC_Plugin plugin{*fixture_descriptor()};
    std::vector<FC_Group> groups{plugin.groups, plugin.groups + plugin.group_count};
    std::vector<FC_Patch> patches{plugin.patches, plugin.patches + plugin.patch_count};
    std::vector<FC_Variant> variants{patches.front().variants, patches.front().variants + patches.front().variant_count};
    std::vector<FC_ImageRequirement> images{variants.front().images, variants.front().images + variants.front().image_count};

    // Builds a mutable declaration with copies of the first patch's variants and the first variant's images.
    MutablePluginGraph() {
        // Redirect the copied descriptor and first variant to test-owned arrays; text still belongs to the fixture DLL.
        plugin.groups = groups.data();
        plugin.patches = patches.data();
        patches.front().variants = variants.data();
        variants.front().images = images.data();
    }
};
} // namespace

TEST_CASE("SDK returns stable declarations and factories own handler construction") {
    PluginModule module{FC_FIXTURE_PLUGIN};
    REQUIRE(module);

    const auto entry = reinterpret_cast<GetPluginFunction>(module.symbol("FC_GetPlugin"));
    const auto active_handler_count = reinterpret_cast<std::uint32_t(__cdecl *)()>(module.symbol("FC_TestActiveHandlerCount"));
    REQUIRE(entry != nullptr);
    REQUIRE(active_handler_count != nullptr);

    CallbackErrorCapture error;
    const FC_Plugin *first{};
    const FC_Plugin *second{};

    REQUIRE(entry(FC_ABI_VERSION, &first, error.sink()) == 1);
    REQUIRE(entry(FC_ABI_VERSION, &second, error.sink()) == 1);

    REQUIRE(first == second);
    REQUIRE(active_handler_count() == 0);

    FC_Handler handler{};
    REQUIRE(first->patches[0].variants[0].factory.create(first->patches[0].variants[0].factory.context, nullptr, nullptr, nullptr, 0,
                                                         &handler, error.sink()) == 1);
    REQUIRE(active_handler_count() == 1);
    REQUIRE(handler.destroy != nullptr);

    handler.destroy(handler.context);
    REQUIRE(active_handler_count() == 0);
}

TEST_CASE("FC_GetPlugin rejects an unsupported ABI and clears the output") {
    PluginModule module{FC_FIXTURE_PLUGIN};
    REQUIRE(module);
    const auto entry = reinterpret_cast<GetPluginFunction>(module.symbol("FC_GetPlugin"));
    REQUIRE(entry != nullptr);

    CallbackErrorCapture error;
    const FC_Plugin *descriptor = reinterpret_cast<const FC_Plugin *>(1);
    // A previous ABI uses a smaller nested variant record and must fail before construction.
    REQUIRE(entry(FC_ABI_VERSION - 1, &descriptor, error.sink()) == 0);
    CHECK(descriptor == nullptr);
    CHECK(error.message.find("ABI") != std::string::npos);
}

TEST_CASE("plugin admission accepts a larger record and rejects an invalid array view") {
    MutablePluginGraph graph;
    graph.plugin.byte_size = sizeof(FC_Plugin) + 32;
    const auto accepted = fc::runtime::admit_definitions({{"larger", &graph.plugin}});
    REQUIRE(accepted.admitted.size() == 1);

    graph.plugin.categories = nullptr;
    const auto rejected = fc::runtime::admit_definitions({{"null-view", &graph.plugin}});
    CHECK(rejected.admitted.empty());
    CHECK(has_diagnostic_containing(rejected, "categories"));
}

TEST_CASE("plugin admission rejects unknown group members and malformed image selectors") {
    MutablePluginGraph graph;
    const FC_Text missing_member{"missing", 7};
    graph.groups.front().members = &missing_member;
    graph.groups.front().member_count = 1;
    const auto member = fc::runtime::admit_definitions({{"unknown-member", &graph.plugin}});
    CHECK(member.admitted.empty());
    CHECK(has_diagnostic_containing(member, "member"));

    graph = {};
    // A wildcard is allowed only at the end of a selector.
    const FC_Text selector{"Known.*.Bad", 11};
    graph.images.front().selectors = &selector;
    graph.images.front().selector_count = 1;
    const auto image = fc::runtime::admit_definitions({{"bad-selector", &graph.plugin}});
    CHECK(image.admitted.empty());
    CHECK(has_diagnostic_containing(image, "selector"));
}

TEST_CASE("plugin admission rejects both ASCII case variants of a duplicate identity") {
    // Reject both ASCII case variants so the result is independent of load order.
    MutablePluginGraph first;
    MutablePluginGraph second;
    second.plugin.id = FC_Text{"FIXTURE_PLUGIN", 14};

    const auto result = fc::runtime::admit_definitions({{"first.dll", &first.plugin}, {"second.dll", &second.plugin}});

    REQUIRE(result.admitted.empty());
    REQUIRE(result.diagnostics.size() == 2);
    REQUIRE(has_diagnostic_containing(result, "first.dll"));
    REQUIRE(has_diagnostic_containing(result, "second.dll"));
}

TEST_CASE("plugin loading reports independent DLL failures and retains an admitted module") {
    fc::tests::TemporaryDirectory directory;
    const auto unicode = directory.path / std::filesystem::path{u8"\u00e9\u6f22"};
    REQUIRE(std::filesystem::create_directory(unicode));
    const auto missing = unicode / "missing.dll";
    const std::array paths{std::filesystem::path{FC_FIXTURE_PLUGIN}, std::filesystem::path{FC_FIXTURE_NO_EXPORT}, missing};

    const auto result = fc::runtime::load_plugins(paths);
    REQUIRE(result.admitted.size() == 1);
    REQUIRE(result.diagnostics.size() == 2);
    CHECK(has_diagnostic_containing(result, "FC_GetPlugin"));
    CHECK(result.diagnostics.back().owner == fc::runtime::path_text(std::filesystem::absolute(missing)));
    CHECK(result.diagnostics.back().windows_error == ERROR_MOD_NOT_FOUND);
    // The accepted declaration borrows DLL storage after load_plugins returns.
    CHECK(GetModuleHandleW(std::filesystem::path{FC_FIXTURE_PLUGIN}.filename().c_str()) != nullptr);
}

TEST_CASE("plugin loading rejects the opposite architecture", "[.wrong-architecture]") {
    // The hidden test receives its separately built DLL through FC_WRONG_ARCH_PLUGIN.
    char *input{};
    std::size_t length{};
    REQUIRE(_dupenv_s(&input, &length, "FC_WRONG_ARCH_PLUGIN") == 0);
    REQUIRE(input);
    const std::string opposite{input};
    std::free(input);
    REQUIRE(std::filesystem::exists(opposite));

    const std::array paths{std::filesystem::path{opposite}};

    const auto result = fc::runtime::load_plugins(paths);

    REQUIRE(result.admitted.empty());
    REQUIRE(has_diagnostic_containing(result, "architecture"));
}

TEST_CASE("runtime loads a plugin built from the relocated SDK", "[.external]") {
    char *input{};
    std::size_t length{};
    REQUIRE(_dupenv_s(&input, &length, "FC_EXTERNAL_PLUGIN") == 0);
    REQUIRE(input);
    const std::string external{input};
    std::free(input);
    REQUIRE(std::filesystem::exists(external));

    const std::array paths{std::filesystem::path{external}};

    const auto result = fc::runtime::load_plugins(paths);

    REQUIRE(result.diagnostics.empty());
    REQUIRE(result.admitted.size() == 1);
}

TEST_CASE("one admission rejects duplicate identities across embedded and external origins") {
    const auto *definition = fixture_descriptor();
    const std::array paths{std::filesystem::path{FC_FIXTURE_PLUGIN}};
    const std::array embedded{fc::runtime::DefinitionCandidate{"embedded", definition}};

    const auto result = fc::runtime::load_plugins(paths, embedded);

    CHECK(result.admitted.empty());
    CHECK(result.diagnostics.size() == 2);
    CHECK(has_diagnostic_containing(result, "conflicts"));
}

TEST_CASE("plugin loading rejects short descriptors and failed entry results") {
    // The probe export chooses its failure mode from the copied DLL's basename.
    fc::tests::TemporaryDirectory directory;
    for (const auto name : {"BadSize.dll", "BadResult.dll"}) {
        const auto path = directory.path / name;
        std::filesystem::copy_file(FC_BOOTSTRAP_PROBE, path, std::filesystem::copy_options::overwrite_existing);

        const std::array paths{path};
        const auto result = fc::runtime::load_plugins(paths);

        CAPTURE(name);
        CHECK(result.admitted.empty());
        CHECK_FALSE(result.diagnostics.empty());

        // Rejected declarations must release the loader reference as well as report the failure.
        const auto wide = path.filename().wstring();
        CHECK(GetModuleHandleW(wide.c_str()) == nullptr);
    }
}

TEST_CASE("a DLL handler factory accepts typed scalars and provides a status callback") {
    // Select the typed-settings variant and feed its factory one effective scalar in schema order.
    const auto *definition = fixture_descriptor();
    const auto &variant = definition->patches[2].variants[0];
    REQUIRE(variant.setting_count == 1);

    FC_Value value{};
    value.kind = FC_SCALAR_UNSIGNED;
    value.value.unsigned_value = 33;
    CallbackErrorCapture error;
    FC_Handler handler{};

    REQUIRE(variant.factory.create(variant.factory.context, nullptr, nullptr, &value, 1, &handler, error.sink()) == 1);
    REQUIRE(handler.status);

    std::string status;
    FC_StatusWriter writer{&status, [](void *context, FC_Text text) -> std::uint32_t {
                               static_cast<std::string *>(context)->assign(text.data, text.size);
                               return 1;
                           }};

    // Observe the constructed field through a lifecycle callback exported by the DLL.
    handler.status(handler.context, &writer);

    CHECK(status == "33");

    handler.destroy(handler.context);

    // One past the uint8_t range must reject construction and clear the previous handler output.
    value.value.unsigned_value = 256;
    CHECK(variant.factory.create(variant.factory.context, nullptr, nullptr, &value, 1, &handler, error.sink()) == 0);
    CHECK(handler.context == nullptr);
}

TEST_CASE("plugin loading unloads a rejected DLL when an embedded plugin shares its source label") {
    fc::tests::TemporaryDirectory directory;
    const auto path = directory.path / "BadSize.dll";
    std::filesystem::copy_file(FC_BOOTSTRAP_PROBE, path, std::filesystem::copy_options::overwrite_existing);
    const std::array paths{path};
    const std::array embedded{
        fc::runtime::DefinitionCandidate{fc::runtime::path_text(std::filesystem::absolute(path)), fixture_descriptor()}};

    const auto result = fc::runtime::load_plugins(paths, embedded);

    REQUIRE(result.admitted.size() == 1);
    CHECK(result.admitted.front().definition == embedded.front().definition);
    CHECK(has_diagnostic_containing(result, "FC_Plugin record supplies"));
    CHECK(GetModuleHandleW(path.filename().c_str()) == nullptr);
}
