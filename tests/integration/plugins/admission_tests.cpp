#include "catalog_builder.hpp"
#include "definition_copy.hpp"
#include "plugin_discovery.hpp"
#include "recognition.hpp"

#include <FusionCutter/SDK.hpp>

#include <catch2/catch_test_macros.hpp>

#include <Windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

// Each integration case gets an isolated discovery directory and unique DLL names so unloading is observable.
class TemporaryDirectory {
  public:
    TemporaryDirectory() {
        static std::atomic_uint32_t sequence;
        path_ = std::filesystem::temp_directory_path() /
                ("FusionCutter-admission-" + std::to_string(GetCurrentProcessId()) + "-" +
                 std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)));
        std::filesystem::create_directories(path_ / "plugins");
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    [[nodiscard]] const std::filesystem::path& root() const noexcept {
        return path_;
    }

    [[nodiscard]] std::filesystem::path plugins() const {
        return path_ / "plugins";
    }

    [[nodiscard]] std::filesystem::path stage(const std::filesystem::path& source, std::wstring_view filename) const {
        const auto destination = plugins() / filename;
        std::filesystem::copy_file(source, destination, std::filesystem::copy_options::overwrite_existing);
        return destination;
    }

  private:
    std::filesystem::path path_;
};

// Shared helpers construct the minimal real host, target, and builder required to cross the native admission path.
[[nodiscard]] const FC_HostApi& host() noexcept {
    static const FC_HostApi value{.struct_size = sizeof(FC_HostApi)};
    return value;
}

[[nodiscard]] fc::catalog::CodeOwner code_owner() {
    auto owner = fc::catalog::CodeOwner::from_address(reinterpret_cast<std::uintptr_t>(&fc::catalog::acquire_catalog));
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

[[nodiscard]] fc::catalog::CatalogBuildResult build(fc::catalog::CatalogBuilder& builder,
                                                    const TemporaryDirectory& directory) {
    auto current_target = target();
    return builder.build(current_target, fc::config::ConfigurationPaths{directory.root()});
}

[[nodiscard]] fc::catalog::CatalogBuilder builder() {
    fc::catalog::CatalogBuilder result{host(), code_owner()};
    result.add_core(fc::catalog::core_registration_bridge());
    return result;
}

[[nodiscard]] bool has_plugin(const fc::catalog::CatalogBuildResult& result, std::string_view id) {
    if (!result.catalog) {
        return false;
    }
    return std::ranges::any_of(result.catalog->plugins(), [&](const auto& plugin) {
        return fc::catalog::equal_ascii_case_insensitive(plugin.definition.id, id);
    });
}

[[nodiscard]] bool module_loaded(const std::filesystem::path& path) noexcept {
    return GetModuleHandleW(path.filename().c_str()) != nullptr;
}

// The sentinel exposes SDK adapter release after rejection without exposing internal registration state.
std::atomic_uint32_t g_bundle_cleanup_count;

struct CleanupSentinel {
    ~CleanupSentinel() noexcept {
        g_bundle_cleanup_count.fetch_add(1, std::memory_order_relaxed);
    }
};

// Factories for bundled plugins retain a sentinel through the Plan callback to prove complete adapter destruction.
fc::Plugin build_colliding_bundle() {
    auto sentinel = std::make_shared<CleanupSentinel>();
    return fc::plugin({
        .id = "AdmissionProbe",
        .patches =
            {
                fc::plan_patch(
                    {
                        .id = "BundledCollisionPatch",
                        .name = "Bundled collision probe",
                        .supports =
                            {
                                fc::support({
                                    .layouts = {fc::TargetLayout::GameSpyRetail},
                                    .roles = fc::HostRole::Client,
                                    .image = fc::TargetImage::Game,
                                }),
                            },
                    },
                    [sentinel = std::move(sentinel)](fc::Plan&) {
                        (void)sentinel;
                    }),
            },
    });
}

// This bundled plugin makes SDK adapter cleanup observable after rejection for malformed configuration.
fc::Plugin build_configurable_bundle() {
    auto sentinel = std::make_shared<CleanupSentinel>();
    return fc::plugin({
        .id = "BundleConfigProbe",
        .patches =
            {
                fc::plan_patch(
                    {
                        .id = "BundleConfigPatch",
                        .name = "Bundled plugin configuration probe",
                        .supports =
                            {
                                fc::support({
                                    .layouts = {fc::TargetLayout::SteamRetail},
                                    .roles = fc::HostRole::Client,
                                    .image = fc::TargetImage::Game,
                                }),
                            },
                    },
                    [sentinel = std::move(sentinel)](fc::Plan&) {
                        (void)sentinel;
                    }),
            },
    });
}

[[nodiscard]] fc::catalog::RegistrationBridge colliding_bundle_bridge() noexcept {
    return {fc::detail::bundled_registration<&build_colliding_bundle>(),
            &fc::detail::release_registration<&build_colliding_bundle>};
}

[[nodiscard]] fc::catalog::RegistrationBridge configurable_bundle_bridge() noexcept {
    return {fc::detail::bundled_registration<&build_configurable_bundle>(),
            &fc::detail::release_registration<&build_configurable_bundle>};
}

void write_file(const std::filesystem::path& path, std::string_view content) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output{path, std::ios::binary};
    REQUIRE(output.is_open());
    output.write(content.data(), static_cast<std::streamsize>(content.size()));
    REQUIRE(output.good());
}

// Changes both architecture markers so discovery sees a consistent image for the other architecture.
void change_to_opposite_architecture(const std::filesystem::path& path) {
    std::fstream file{path, std::ios::binary | std::ios::in | std::ios::out};
    REQUIRE(file.is_open());
    IMAGE_DOS_HEADER dos{};
    file.read(reinterpret_cast<char*>(&dos), sizeof(dos));
    REQUIRE(file.good());
    REQUIRE(dos.e_magic == IMAGE_DOS_SIGNATURE);
    const auto file_header_offset = static_cast<std::streamoff>(dos.e_lfanew) + sizeof(DWORD);
    file.seekg(file_header_offset);
    IMAGE_FILE_HEADER header{};
    file.read(reinterpret_cast<char*>(&header), sizeof(header));
    REQUIRE(file.good());
    const auto optional_offset = file_header_offset + sizeof(IMAGE_FILE_HEADER);
    const auto section_offset = optional_offset + header.SizeOfOptionalHeader;
    std::vector<IMAGE_SECTION_HEADER> sections(header.NumberOfSections);
    file.seekg(section_offset);
    file.read(reinterpret_cast<char*>(sections.data()),
              static_cast<std::streamsize>(sections.size() * sizeof(IMAGE_SECTION_HEADER)));
    REQUIRE(file.good());

    IMAGE_DATA_DIRECTORY exports{};
    DWORD size_of_headers{};
    file.seekg(optional_offset);
    if constexpr (sizeof(void*) == 4) {
        IMAGE_OPTIONAL_HEADER32 current{};
        file.read(reinterpret_cast<char*>(&current), sizeof(current));
        exports = current.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        size_of_headers = current.SizeOfHeaders;
        IMAGE_OPTIONAL_HEADER64 opposite{};
        opposite.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
        opposite.SizeOfHeaders = size_of_headers;
        opposite.NumberOfRvaAndSizes = IMAGE_NUMBEROF_DIRECTORY_ENTRIES;
        opposite.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT] = exports;
        header.Machine = IMAGE_FILE_MACHINE_AMD64;
        header.SizeOfOptionalHeader = static_cast<WORD>(sizeof(opposite));
        file.seekp(optional_offset);
        file.write(reinterpret_cast<const char*>(&opposite), sizeof(opposite));
    } else {
        IMAGE_OPTIONAL_HEADER64 current{};
        file.read(reinterpret_cast<char*>(&current), sizeof(current));
        exports = current.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        size_of_headers = current.SizeOfHeaders;
        IMAGE_OPTIONAL_HEADER32 opposite{};
        opposite.Magic = IMAGE_NT_OPTIONAL_HDR32_MAGIC;
        opposite.SizeOfHeaders = size_of_headers;
        opposite.NumberOfRvaAndSizes = IMAGE_NUMBEROF_DIRECTORY_ENTRIES;
        opposite.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT] = exports;
        header.Machine = IMAGE_FILE_MACHINE_I386;
        header.SizeOfOptionalHeader = static_cast<WORD>(sizeof(opposite));
        file.seekp(optional_offset);
        file.write(reinterpret_cast<const char*>(&opposite), sizeof(opposite));
    }
    // The optional-header size moves the section table; raw section offsets remain those of the unchanged file data.
    file.seekp(file_header_offset);
    file.write(reinterpret_cast<const char*>(&header), sizeof(header));
    file.seekp(optional_offset + header.SizeOfOptionalHeader);
    file.write(reinterpret_cast<const char*>(sections.data()),
               static_cast<std::streamsize>(sections.size() * sizeof(IMAGE_SECTION_HEADER)));
    REQUIRE(file.good());
}

// Leaves PE32/PE32+ unchanged to prove contradictory architecture metadata is rejected as malformed input.
void change_machine_only(const std::filesystem::path& path) {
    std::fstream file{path, std::ios::binary | std::ios::in | std::ios::out};
    REQUIRE(file.is_open());
    IMAGE_DOS_HEADER dos{};
    file.read(reinterpret_cast<char*>(&dos), sizeof(dos));
    REQUIRE(file.good());
    REQUIRE(dos.e_magic == IMAGE_DOS_SIGNATURE);
    file.seekp(static_cast<std::streamoff>(dos.e_lfanew) + sizeof(DWORD));
    const WORD machine = sizeof(void*) == 4 ? IMAGE_FILE_MACHINE_AMD64 : IMAGE_FILE_MACHINE_I386;
    file.write(reinterpret_cast<const char*>(&machine), sizeof(machine));
    REQUIRE(file.good());
}

} // namespace

TEST_CASE("the ABI probe admits compatible table forms and unloads with provisional ownership") {
    const std::vector<std::wstring> filenames{L"Valid.dll", L"LargeTable.dll", L"OldSdk.dll"};
    for (const auto& filename : filenames) {
        TemporaryDirectory directory;
        const auto plugin_path = directory.stage(FC_ABI_PROBE_PATH, filename);
        {
            auto current_builder = builder();
            auto result = build(current_builder, directory);
            INFO("ABI probe: " << plugin_path.string());
            REQUIRE(result.catalog.has_value());
            CHECK(has_plugin(result, "AdmissionProbe"));
            CHECK(module_loaded(plugin_path));
        }
        CHECK_FALSE(module_loaded(plugin_path));
    }
}

TEST_CASE("the ABI probe rejects malformed query, registration, and structural forms as whole plugins") {
    // Filenames script one native DLL through distinct defects and identify the boundary that must reject each form.
    struct Row {
        std::wstring filename;
        fc::catalog::AdmissionStage stage;
    };
    const std::vector<Row> rows{
        {L"NoSubmission.dll", fc::catalog::AdmissionStage::Registration},
        {L"MultipleSubmission.dll", fc::catalog::AdmissionStage::Registration},
        {L"RegistrationFailure.dll", fc::catalog::AdmissionStage::Registration},
        {L"InvalidStructure.dll", fc::catalog::AdmissionStage::Structure},
        {L"SmallTable.dll", fc::catalog::AdmissionStage::Query},
        {L"HostTooLarge.dll", fc::catalog::AdmissionStage::Query},
        {L"NullRegister.dll", fc::catalog::AdmissionStage::Query},
    };
    for (const auto& row : rows) {
        TemporaryDirectory directory;
        const auto plugin_path = directory.stage(FC_ABI_PROBE_PATH, row.filename);
        auto current_builder = builder();
        auto result = build(current_builder, directory);
        INFO("ABI rejection probe: " << plugin_path.string());
        REQUIRE(result.catalog.has_value());
        CHECK(result.catalog->plugins().size() == 1);
        REQUIRE(result.rejections.size() == 1);
        CHECK(result.rejections.front().stage == row.stage);
        CHECK(result.rejections.front().path == plugin_path);
        CHECK_FALSE(result.rejections.front().reason.empty());
        CHECK_FALSE(module_loaded(plugin_path));
    }
}

TEST_CASE("external discovery order is deterministic") {
    // Stage names in reverse order so order in the plugin catalog can only come from discovery's filename policy.
    TemporaryDirectory directory;
    (void)directory.stage(FC_ABI_PROBE_PATH, L"Zulu.dll");
    (void)directory.stage(FC_ABI_PROBE_PATH, L"Alpha.dll");

    auto current_builder = builder();
    auto result = build(current_builder, directory);
    REQUIRE(result.catalog.has_value());
    REQUIRE(result.catalog->plugins().size() == 3);
    CHECK(result.catalog->plugins()[0].definition.id == "Core");
    CHECK(result.catalog->plugins()[1].definition.id == "AlphaProbe");
    CHECK(result.catalog->plugins()[2].definition.id == "ZuluProbe");
}

TEST_CASE("collisions without the built-in Core plugin reject every participant and clean adapter state") {
    // Two external plugins and one bundled plugin intentionally claim the same global plugin identity.
    TemporaryDirectory directory;
    const auto first = directory.stage(FC_ABI_PROBE_PATH, L"CollisionOne.dll");
    const auto second = directory.stage(FC_ABI_PROBE_PATH, L"CollisionTwo.dll");
    g_bundle_cleanup_count.store(0, std::memory_order_relaxed);

    auto current_builder = builder();
    current_builder.add_bundled(colliding_bundle_bridge());
    auto result = build(current_builder, directory);
    // Collision removal must unload DLLs and release the bundled plugin adapter even when no participant survives.
    REQUIRE(result.catalog.has_value());
    CHECK(result.catalog->plugins().size() == 1);
    CHECK(result.rejections.size() == 3);
    CHECK(std::ranges::all_of(result.rejections, [](const auto& rejection) {
        return rejection.stage == fc::catalog::AdmissionStage::Collision && rejection.plugin_id.has_value() &&
               !rejection.reason.empty();
    }));
    CHECK(g_bundle_cleanup_count.load(std::memory_order_relaxed) == 1);
    CHECK_FALSE(module_loaded(first));
    CHECK_FALSE(module_loaded(second));
}

TEST_CASE("the generated SDK adapter crosses the real DLL registration boundary") {
    TemporaryDirectory directory;
    const auto plugin_path = directory.stage(FC_SDK_PROBE_PATH, L"SdkGenerated.dll");
    auto current_builder = builder();
    auto result = build(current_builder, directory);

    REQUIRE(result.catalog.has_value());
    CHECK(has_plugin(result, "SdkFixture"));
    CHECK(module_loaded(plugin_path));
}

TEST_CASE("whole-file configuration failures reject external and bundled plugins before final transfer") {
    // Both origins must release their distinct code/state owners at the same configuration gate.
    SECTION("external") {
        TemporaryDirectory directory;
        const auto plugin_path = directory.stage(FC_ABI_PROBE_PATH, L"ConfigurationFailure.dll");
        write_file(directory.root() / "config" / "FC.ConfigurationFailureProbe.ini", "[General]\rBroken=true\n");

        auto current_builder = builder();
        auto result = build(current_builder, directory);
        REQUIRE(result.catalog.has_value());
        CHECK_FALSE(has_plugin(result, "ConfigurationFailureProbe"));
        REQUIRE(result.rejections.size() == 1);
        CHECK(result.rejections.front().stage == fc::catalog::AdmissionStage::Configuration);
        CHECK_FALSE(module_loaded(plugin_path));
    }

    SECTION("bundled") {
        TemporaryDirectory directory;
        write_file(directory.root() / "config" / "FC.BundleConfigProbe.ini", "[General]\rBroken=true\n");
        g_bundle_cleanup_count.store(0, std::memory_order_relaxed);

        auto current_builder = builder();
        current_builder.add_bundled(configurable_bundle_bridge());
        auto result = build(current_builder, directory);
        REQUIRE(result.catalog.has_value());
        CHECK_FALSE(has_plugin(result, "BundleConfigProbe"));
        REQUIRE(result.rejections.size() == 1);
        CHECK(result.rejections.front().stage == fc::catalog::AdmissionStage::Configuration);
        CHECK(g_bundle_cleanup_count.load(std::memory_order_relaxed) == 1);
    }
}

TEST_CASE("plugin-local dependent DLLs load only from the approved search locations") {
    // Staging both binaries together exercises the loader's scoped search in the plugin directory.
    TemporaryDirectory directory;
    const auto dependency = directory.stage(FC_DEPENDENCY_PATH, L"fc_probe_dependency.dll");
    const auto plugin = directory.stage(FC_DEPENDENT_PROBE_PATH, L"DependentPlugin.dll");
    {
        auto current_builder = builder();
        auto result = build(current_builder, directory);
        REQUIRE(result.catalog.has_value());
        CHECK(has_plugin(result, "DependentProbe"));
        CHECK(module_loaded(plugin));
        CHECK(module_loaded(dependency));
    }
    CHECK_FALSE(module_loaded(plugin));
    CHECK_FALSE(module_loaded(dependency));
}

TEST_CASE("forwarded exports and wrong architecture are rejected before execution") {
    TemporaryDirectory directory;
    const auto forwarded = directory.stage(FC_FORWARDER_PROBE_PATH, L"Forwarded.dll");
    const auto wrong_architecture = directory.stage(FC_ABI_PROBE_PATH, L"WrongArchitecture.dll");
    change_to_opposite_architecture(wrong_architecture);

    // Inspection rejects both files without loading either image into the process.
    const auto discovery = fc::catalog::discover_plugins(directory.plugins());
    CHECK(discovery.candidates.empty());
    REQUIRE(discovery.rejections.size() == 2);
    CHECK(std::ranges::any_of(discovery.rejections, [](const auto& rejection) {
        return rejection.stage == fc::catalog::AdmissionStage::Architecture;
    }));
    CHECK(std::ranges::all_of(discovery.rejections, [](const auto& rejection) {
        return rejection.path.has_value() && !rejection.reason.empty();
    }));
    CHECK_FALSE(module_loaded(forwarded));
    CHECK_FALSE(module_loaded(wrong_architecture));
}

TEST_CASE("contradictory PE architecture markers are rejected before execution") {
    TemporaryDirectory directory;
    const auto malformed = directory.stage(FC_ABI_PROBE_PATH, L"MalformedArchitecture.dll");
    change_machine_only(malformed);

    const auto discovery = fc::catalog::discover_plugins(directory.plugins());
    CHECK(discovery.candidates.empty());
    REQUIRE(discovery.rejections.size() == 1);
    CHECK(discovery.rejections.front().stage == fc::catalog::AdmissionStage::Discovery);
    CHECK(discovery.rejections.front().reason.find("architecture disagree") != std::string::npos);
    CHECK_FALSE(module_loaded(malformed));
}

TEST_CASE("only the first 128 query exports with the exact required name consume candidate slots") {
    TemporaryDirectory directory;
    std::filesystem::path first;
    // Hard links keep the fixture cheap while presenting distinct deterministically sorted candidate filenames.
    for (std::size_t index = 0; index < fc::catalog::kExternalCandidateCapacity + 1; ++index) {
        const auto name = L"Candidate" + std::to_wstring(index) + L".dll";
        const auto destination = directory.plugins() / name;
        if (index == 0) {
            first = directory.stage(FC_ABI_PROBE_PATH, name);
            continue;
        }
        std::error_code error;
        std::filesystem::create_hard_link(first, destination, error);
        if (error) {
            std::filesystem::copy_file(first, destination);
        }
    }

    const auto discovery = fc::catalog::discover_plugins(directory.plugins());
    // Retain the sorted prefix as candidates and reject only the next exactly named export for capacity.
    CHECK(discovery.candidates.size() == fc::catalog::kExternalCandidateCapacity);
    CHECK(std::ranges::count_if(discovery.rejections, [](const auto& rejection) {
              return rejection.stage == fc::catalog::AdmissionStage::Capacity;
          }) == 1);
}
