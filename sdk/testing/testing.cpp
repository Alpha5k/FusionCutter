#include <FusionCutter/Testing.hpp>

#include "../../src/core/catalog/catalog_builder.hpp"
#include "../../src/core/catalog/definition_copy.hpp"
#include "../../src/core/catalog/plugin_discovery.hpp"
#include "../../src/core/planning/plan_validation.hpp"
#include "../../src/core/planning/resolution.hpp"
#include "../../src/core/targets/mapped_file.hpp"
#include "../../src/core/targets/recognition.hpp"
#include "../../src/core/targets/target_profiles.hpp"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <concepts>
#include <cstring>
#include <format>
#include <ranges>
#include <type_traits>
#include <utility>

namespace fc::test {
namespace {

// Scenario errors keep a human explanation and the owning high-level operation separate for CLI reporting.
[[nodiscard]] Error scenario_error(std::string message, std::string operation) {
    return {.message = std::move(message), .operation = std::move(operation)};
}

[[nodiscard]] bool same_id(std::string_view left, std::string_view right) noexcept {
    // Public result lookup follows the ASCII ID rule without depending on private storage for the plugin catalog.
    if (left.size() != right.size()) {
        return false;
    }
    const auto fold = [](unsigned char value) {
        return value >= 'A' && value <= 'Z' ? static_cast<unsigned char>(value + ('a' - 'A')) : value;
    };
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (fold(static_cast<unsigned char>(left[index])) != fold(static_cast<unsigned char>(right[index]))) {
            return false;
        }
    }
    return true;
}

// TemporaryDirectory confines every ordinary configuration write to test-owned disposable storage.
class TemporaryDirectory final {
  public:
    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;
    TemporaryDirectory(TemporaryDirectory&& other) noexcept : path_(std::exchange(other.path_, {})) {}

    TemporaryDirectory& operator=(TemporaryDirectory&& other) noexcept {
        if (this != &other) {
            std::error_code ignored;
            std::filesystem::remove_all(path_, ignored);
            path_ = std::exchange(other.path_, {});
        }
        return *this;
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] static std::expected<TemporaryDirectory, Error> create() {
        // Resolve the system temporary root once before attempting collision-resistant private names.
        static std::atomic_uint64_t sequence{};
        std::error_code error;
        const auto root = std::filesystem::temp_directory_path(error);
        if (error) {
            return std::unexpected(
                scenario_error("The temporary directory root is unavailable", "Create scenario workspace"));
        }
        // Bound retries so hostile or unavailable temporary storage becomes an explicit Scenario failure.
        for (unsigned attempt = 0; attempt < 32; ++attempt) {
            const auto name = std::format("FusionCutter-Scenario-{}-{}", GetCurrentProcessId(),
                                          sequence.fetch_add(1, std::memory_order_relaxed));
            auto path = root / name;
            if (std::filesystem::create_directory(path, error)) {
                return TemporaryDirectory{std::move(path)};
            }
            if (error && error != std::errc::file_exists) {
                break;
            }
            error.clear();
        }
        return std::unexpected(
            scenario_error("A private scenario directory could not be created", "Create scenario workspace"));
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

  private:
    explicit TemporaryDirectory(std::filesystem::path path) : path_(std::move(path)) {}

    std::filesystem::path path_;
};

[[nodiscard]] std::expected<void, Error> stage_configuration(const std::optional<std::filesystem::path>& source,
                                                             const std::filesystem::path& workspace) {
    // Ordinary configuration parsing may generate defaults, so all activity is redirected into private copied input.
    const auto destination = workspace / "config";
    std::error_code error;
    std::filesystem::create_directory(destination, error);
    if (error) {
        return std::unexpected(
            scenario_error("The private configuration directory could not be created", "Stage configuration"));
    }
    if (!source) {
        return {};
    }
    if (!std::filesystem::is_directory(*source, error) || error) {
        return std::unexpected(
            scenario_error("The source configuration path is not a readable directory", "Stage configuration"));
    }
    // Copy each entry beneath config so passing a directory never introduces an extra basename level.
    for (std::filesystem::directory_iterator iterator{*source, error}, end; iterator != end && !error;
         iterator.increment(error)) {
        std::filesystem::copy(
            iterator->path(), destination / iterator->path().filename(),
            std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing, error);
        if (error) {
            break;
        }
    }
    if (error) {
        return std::unexpected(
            scenario_error("The source configuration could not be copied completely", "Stage configuration"));
    }
    return {};
}

// Scenario validation accepts only images matching the verifier process that will execute plugin callbacks.
[[nodiscard]] constexpr FC_Architecture process_architecture() noexcept {
    return sizeof(void*) == 4 ? FC_ARCH_X86 : FC_ARCH_X64;
}

template <class ImageInput>
[[nodiscard]] std::expected<targets::RecognizedTarget, Error> build_target(TargetLayout layout, HostRole role,
                                                                           std::span<const ImageInput> inputs) {
    // Validate every explicit identity against the requested tuple before any image enters shared planning state.
    if (role != HostRole::Client && role != HostRole::Server) {
        return std::unexpected(scenario_error("The scenario role must be Client or Server", "Build scenario target"));
    }
    std::vector<targets::OwnedImage> startup;
    std::vector<targets::OwnedImage> late;
    std::vector<TargetImage> seen;
    for (const auto& input : inputs) {
        if (std::ranges::find(seen, input.image) != seen.end()) {
            return std::unexpected(
                scenario_error("The scenario contains a duplicate target image ID", "Build scenario target"));
        }
        seen.push_back(input.image);
        if (!catalog::valid_framework_id(input.image_profile)) {
            return std::unexpected(
                scenario_error("The supplied image profile is not a valid framework ID", "Build scenario target"));
        }
        const auto* profile = targets::find_image_profile(input.image_profile);
        if (profile == nullptr || profile->layout != static_cast<FC_TargetLayout>(layout) ||
            profile->image != static_cast<FC_TargetImage>(input.image) ||
            profile->architecture != process_architecture()) {
            return std::unexpected(
                scenario_error("The supplied image profile is outside the scenario tuple", "Build scenario target"));
        }

        // Files become private image copies; borrowed spans are copied to protect caller storage.
        std::expected<std::vector<std::byte>, std::string> bytes = std::visit(
            [](const auto& source) -> std::expected<std::vector<std::byte>, std::string> {
                using Source = std::remove_cvref_t<decltype(source)>;
                if constexpr (std::same_as<Source, std::filesystem::path>) {
                    return targets::map_pe_file(source);
                } else {
                    return std::vector<std::byte>{source.begin(), source.end()};
                }
            },
            input.source);
        if (!bytes) {
            return std::unexpected(scenario_error(std::move(bytes.error()), "Map scenario image"));
        }
        // Scenario derives safe image policy from PE structure but never infers the caller's explicit identity again.
        auto image = targets::validate_synthetic_mapped_image(*profile, std::move(*bytes));
        if (!image) {
            return std::unexpected(scenario_error(std::move(image.error().message), "Validate scenario image"));
        }
        (profile->late ? late : startup).push_back(std::move(*image));
    }

    // The production target owner enforces startup image combinations and the role-specific policy for late images.
    auto target = targets::RecognizedTarget::create(static_cast<FC_HostRole>(role), std::move(startup));
    if (!target || target->layout() != static_cast<FC_TargetLayout>(layout)) {
        return std::unexpected(scenario_error(target ? "The startup images do not match the requested layout"
                                                     : std::move(target.error().message),
                                              "Build scenario target"));
    }
    for (auto& image : late) {
        if (!target->add_late_image(std::move(image))) {
            return std::unexpected(scenario_error("The supplied late image is outside the approved scenario tuple",
                                                  "Build scenario target"));
        }
    }
    return std::move(*target);
}

// Scenario lifecycle callbacks receive a complete inert host table, but runtime-only logging and tracing stay absent.
FC_Bool FC_CALL log_disabled(void*, FC_ReportToken, FC_LogLevel) noexcept {
    return FC_FALSE;
}

void FC_CALL discard_log(void*, FC_ReportToken, FC_LogLevel, FC_StringView) noexcept {}

FC_Bool FC_CALL trace_disabled(void*, FC_TraceHandle) noexcept {
    return FC_FALSE;
}

FC_Bool FC_CALL discard_trace(void*, FC_TraceHandle, FC_ByteView) noexcept {
    return FC_FALSE;
}

void FC_CALL clear_trace_health(void*, FC_TraceHandle, FC_TraceHealth* output) noexcept {
    if (output == nullptr) {
        return;
    }
    constexpr auto required_size = offsetof(FC_TraceHealth, output_failed) + sizeof(output->output_failed);
    const auto requested = output->struct_size;
    if (requested < required_size) {
        return;
    }
    const FC_TraceHealth empty{.struct_size = sizeof(FC_TraceHealth)};
    std::memcpy(output, &empty, std::min<std::size_t>(requested, sizeof(empty)));
}

[[nodiscard]] PatchState public_state(planning::PatchState state) noexcept {
    // A completed Scenario run cannot expose the internal Pending or runtime Installed states.
    switch (state) {
    case planning::PatchState::Disabled:
        return PatchState::Disabled;
    case planning::PatchState::NotApplicable:
        return PatchState::NotApplicable;
    case planning::PatchState::WaitingForImage:
        return PatchState::WaitingForImage;
    case planning::PatchState::Ready:
        return PatchState::Ready;
    case planning::PatchState::Skipped:
        return PatchState::Skipped;
    case planning::PatchState::Failed:
        return PatchState::Failed;
    case planning::PatchState::Pending:
    case planning::PatchState::Installed:
        return PatchState::Failed;
    }
    return PatchState::Failed;
}

[[nodiscard]] std::optional<PatchPhase> public_phase(const std::optional<planning::PatchPhase>& phase) noexcept {
    if (!phase || *phase > planning::PatchPhase::Validation) {
        return std::nullopt;
    }
    return static_cast<PatchPhase>(static_cast<std::underlying_type_t<planning::PatchPhase>>(*phase));
}

[[nodiscard]] std::uint64_t operation_size(const planning::SubmittedPlan& plan,
                                           const planning::OperationRecord& operation) noexcept {
    // A validated write claim is authoritative when present; non-memory operations retain their own semantic extent.
    const auto claim = std::ranges::find_if(plan.claims, [&](const auto& candidate) {
        return candidate.operation_index == operation.index && candidate.access == planning::ClaimAccess::Write;
    });
    if (claim != plan.claims.end()) {
        return claim->size;
    }
    return std::visit(
        [](const auto& payload) -> std::uint64_t {
            using Payload = std::remove_cvref_t<decltype(payload)>;
            if constexpr (std::same_as<Payload, planning::RequireOperation>) {
                return payload.size;
            } else if constexpr (std::same_as<Payload, planning::NopOperation>) {
                return payload.size;
            } else if constexpr (std::same_as<Payload, planning::DataAllocationOperation>) {
                return payload.byte_size;
            } else if constexpr (std::same_as<Payload, planning::InterfaceBindingOperation>) {
                return payload.size;
            } else if constexpr (std::same_as<Payload, planning::HookOperation>) {
                return payload.overwrite_size;
            } else if constexpr (std::same_as<Payload, planning::WriteOperation>) {
                return payload.bytes.size();
            } else {
                return 5;
            }
        },
        operation.payload);
}

[[nodiscard]] Operation copy_operation(const planning::SubmittedPlan& plan, const planning::OperationRecord& operation,
                                       FC_TargetImage support_image) {
    // Variant lowering deliberately copies only the stable assertion surface promised by the public Testing API.
    Operation result{.operation_index = operation.index, .byte_size = operation_size(plan, operation)};
    std::visit(
        [&](const auto& payload) {
            using Payload = std::remove_cvref_t<decltype(payload)>;
            if constexpr (std::same_as<Payload, planning::RequireOperation>) {
                result.kind = OperationKind::Require;
                result.image = static_cast<TargetImage>(support_image);
                result.rva = Rva{payload.location.rva};
                result.has_evidence = payload.location.evidence.kind != FC_EVIDENCE_NONE;
            } else if constexpr (std::same_as<Payload, planning::WriteOperation>) {
                result.kind = OperationKind::Write;
                result.image = static_cast<TargetImage>(support_image);
                result.rva = Rva{payload.location.rva};
                result.has_evidence = payload.location.evidence.kind != FC_EVIDENCE_NONE;
            } else if constexpr (std::same_as<Payload, planning::NopOperation>) {
                result.kind = OperationKind::Nop;
                result.image = static_cast<TargetImage>(support_image);
                result.rva = Rva{payload.location.rva};
                result.has_evidence = payload.location.evidence.kind != FC_EVIDENCE_NONE;
            } else if constexpr (std::same_as<Payload, planning::RedirectOperation>) {
                result.kind = OperationKind::Redirect;
                result.image = static_cast<TargetImage>(support_image);
                result.rva = Rva{payload.location.rva};
                result.has_evidence = payload.location.evidence.kind != FC_EVIDENCE_NONE;
            } else if constexpr (std::same_as<Payload, planning::DataAllocationOperation>) {
                result.kind = OperationKind::AllocateData;
            } else if constexpr (std::same_as<Payload, planning::InterfaceBindingOperation>) {
                result.kind = OperationKind::BindInterface;
            } else if constexpr (std::same_as<Payload, planning::HookOperation>) {
                result.kind = payload.observer ? OperationKind::Observe : OperationKind::Hook;
                result.image = static_cast<TargetImage>(support_image);
                result.rva = Rva{payload.location.rva};
                result.has_evidence = payload.location.evidence.kind != FC_EVIDENCE_NONE;
            }
        },
        operation.payload);
    return result;
}

// The public result owns no views into the plugin catalog, plugin callbacks, or temporary scenario storage.
struct OwnedResults {
    std::vector<PluginResult> plugins;
    std::vector<PatchResult> patches;
};

// Copies every result before temporary instances and external DLL owners leave scope at the end of validation.
[[nodiscard]] OwnedResults copy_result(const catalog::CatalogBuildResult& built, const planning::PatchWorkSet& work) {
    OwnedResults result;
    const auto& catalog = *built.catalog;
    result.plugins.reserve(catalog.plugins().size() + built.rejections.size());
    // Plugin admission and rejection remain distinct so colliding rejected IDs stay inspectable by path.
    for (const auto& plugin : catalog.plugins()) {
        result.plugins.push_back({.path = plugin.library ? std::optional{plugin.library->path()} : std::nullopt,
                                  .id = plugin.definition.id,
                                  .admitted = true});
    }
    for (const auto& rejection : built.rejections) {
        result.plugins.push_back(
            {.path = rejection.path, .id = rejection.plugin_id, .admitted = false, .reason = rejection.reason});
    }

    result.patches.reserve(work.records().size());
    // Patch outcomes retain shallow failure context plus validated operations and claims, never private transactions.
    for (const auto& record : work.records()) {
        const auto& definition = catalog.patch(record.patch);
        PatchResult copied{.id = definition.id, .state = public_state(record.state)};
        if (record.reason) {
            copied.reason = record.reason->message;
            copied.phase = public_phase(record.reason->phase);
            copied.operation = record.reason->operation;
            copied.related_patch = record.reason->related_patch.transform([](std::string_view value) {
                return std::string{value};
            });
            copied.related_group = record.reason->related_group.transform([](std::string_view value) {
                return std::string{value};
            });
        }
        if (definition.selected_support) {
            const auto image = definition.supports[*definition.selected_support].image;
            copied.operations.reserve(record.plan.operations.size());
            for (const auto& operation : record.plan.operations) {
                copied.operations.push_back(copy_operation(record.plan, operation, image));
            }
        }
        copied.claims.reserve(record.plan.claims.size());
        for (const auto& claim : record.plan.claims) {
            copied.claims.push_back(
                {.image = static_cast<TargetImage>(claim.image),
                 .rva = Rva{claim.rva},
                 .byte_size = claim.size,
                 .access = claim.access == planning::ClaimAccess::Read ? ClaimAccess::Read : ClaimAccess::Write,
                 .operation_index = claim.operation_index});
        }
        result.patches.push_back(std::move(copied));
    }
    return result;
}

} // namespace

std::span<const PluginResult> ScenarioResult::plugins() const noexcept {
    return plugins_;
}

std::span<const PatchResult> ScenarioResult::patches() const noexcept {
    return patches_;
}

const PluginResult* ScenarioResult::find_plugin(std::string_view id) const noexcept {
    const auto found = std::ranges::find_if(plugins_, [&](const auto& plugin) {
        return plugin.admitted && plugin.id && same_id(*plugin.id, id);
    });
    return found == plugins_.end() ? nullptr : &*found;
}

const PatchResult* ScenarioResult::find_patch(std::string_view id) const noexcept {
    const auto found = std::ranges::find_if(patches_, [&](const auto& patch) {
        return same_id(patch.id, id);
    });
    return found == patches_.end() ? nullptr : &*found;
}

Scenario::Scenario(TargetLayout layout, HostRole role) : layout_(layout), role_(role) {}

void Scenario::add_plugin(std::filesystem::path path) {
    plugins_.push_back(std::move(path));
}

void Scenario::add_image(TargetImage image, std::string image_profile, std::filesystem::path path) {
    images_.push_back({image, std::move(image_profile), std::move(path)});
}

void Scenario::add_image(TargetImage image, std::string image_profile, std::span<const std::byte> bytes) {
    images_.push_back({image, std::move(image_profile), bytes});
}

void Scenario::use_config(std::filesystem::path directory) {
    config_directory_ = std::move(directory);
}

std::expected<ScenarioResult, Error> Scenario::validate() const {
    // Normalize author-controlled paths and construct explicit target ownership before loading any plugin code.
    auto normalized_plugins = catalog::normalize_plugin_paths(plugins_);
    if (!normalized_plugins) {
        return std::unexpected(scenario_error(std::move(normalized_plugins.error()), "Normalize plugin inputs"));
    }
    auto target = build_target(layout_, role_, std::span{images_});
    if (!target) {
        return std::unexpected(std::move(target.error()));
    }
    // Configuration generation and parsing are confined to a disposable workspace copied from optional fixtures.
    auto workspace = TemporaryDirectory::create();
    if (!workspace) {
        return std::unexpected(std::move(workspace.error()));
    }
    if (auto staged = stage_configuration(config_directory_, workspace->path()); !staged) {
        return std::unexpected(std::move(staged.error()));
    }

    // This inert host preserves the production ABI while intentionally providing no runtime-only diagnostics services.
    const FC_HostApi host{.struct_size = sizeof(FC_HostApi),
                          .log_enabled = &log_disabled,
                          .log_write = &discard_log,
                          .trace_enabled = &trace_disabled,
                          .trace_try_write = &discard_trace,
                          .trace_health = &clear_trace_health};
    // Plugin admission through common validation matches production except for explicit input acquisition.
    auto built = catalog::acquire_catalog_explicit(host, *target, workspace->path(), *normalized_plugins);
    if (built.fatal_error || !built.catalog || !built.configuration) {
        return std::unexpected(
            scenario_error(built.fatal_error.value_or("Plugin admission did not produce a complete scenario"),
                           "Build scenario plugin catalog"));
    }
    auto work = planning::resolve_patches(
        {.catalog = *built.catalog, .target = *target, .configuration = *built.configuration});
    static_cast<void>(planning::build_installation_plan(*target, work));
    // Result ownership is complete before work records, the plugin catalog, DLLs, and temporary files unwind.
    auto copied = copy_result(built, work);
    ScenarioResult result;
    result.plugins_ = std::move(copied.plugins);
    result.patches_ = std::move(copied.patches);
    return result;
}

} // namespace fc::test
