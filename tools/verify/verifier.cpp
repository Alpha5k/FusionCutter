#include "verifier.hpp"

#include "patching/patching.hpp"
#include "patching/plan_verification.hpp"
#include "targets/layouts/profiles.hpp"
#include "targets/recognition.hpp"

#include <FusionCutter/patch.hpp>

#include <cstddef>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace fusioncutter::verify {
namespace {

using patching_detail::CriticalDependency;
using patching_detail::PlanVerification;

struct PlanCandidate {
    PatchInstance instance;
    PatchPlan plan;
    std::vector<CriticalDependency> dependencies;
};

struct SettingsCase {
    std::optional<std::size_t> index;
    std::string value;
};

class ByteMutation {
  public:
    ByteMutation(std::byte& target, std::byte mutation) noexcept : target_(&target), original_(target) {
        target ^= mutation;
    }

    ByteMutation(const ByteMutation&) = delete;
    ByteMutation& operator=(const ByteMutation&) = delete;

    ~ByteMutation() {
        *target_ = original_;
    }

  private:
    std::byte* target_;
    std::byte original_;
};

[[nodiscard]] std::string describe(const OutcomeReason& reason) {
    if (!reason.operation.has_value()) {
        return reason.message;
    }
    return *reason.operation + ": " + reason.message;
}

[[nodiscard]] std::vector<SettingsCase> verification_cases(const SettingsDefinition& definition) {
    std::vector<SettingsCase> cases{SettingsCase{}};
    const auto metadata = definition.metadata();
    for (std::size_t index = 0; index < metadata.size(); ++index) {
        const auto& setting = metadata[index];
        if (setting.kind == SettingKind::Boolean) {
            for (const std::string_view value : {"0", "1"}) {
                if (value != setting.default_value) {
                    cases.push_back({index, std::string(value)});
                }
            }
        } else if (setting.kind == SettingKind::Choice) {
            for (const auto& value : setting.choices) {
                if (value != setting.default_value) {
                    cases.push_back({index, value});
                }
            }
        }
    }
    return cases;
}

[[nodiscard]] std::string describe(const SettingsDefinition& definition, const SettingsCase& settings_case) {
    if (!settings_case.index.has_value()) {
        return "default settings";
    }

    const auto& setting = definition.metadata()[*settings_case.index];
    const auto key = setting.group.empty() ? setting.key : setting.group + "." + setting.key;
    return key + "=" + settings_case.value;
}

[[nodiscard]] std::expected<ResolvedSettings, std::string> resolve_settings(const SettingsDefinition& definition,
                                                                            const SettingsCase& settings_case) {
    auto settings = definition.make_defaults();
    if (settings_case.index.has_value()) {
        if (auto applied = definition.apply(settings, *settings_case.index, settings_case.value);
            !applied.has_value()) {
            return std::unexpected(describe(applied.error()));
        }
    }
    if (auto validated = definition.validate(settings); !validated.has_value()) {
        return std::unexpected(describe(validated.error()));
    }
    return settings;
}

[[nodiscard]] std::expected<std::optional<PlanCandidate>, std::string>
build_candidate(const catalog::CatalogEntry& entry, const PatchVariant& variant, const TargetContext& target,
                const SettingsCase& settings_case) {
    auto settings = resolve_settings(catalog::variant_settings(entry.definition, variant), settings_case);
    if (!settings.has_value()) {
        return std::unexpected(std::move(settings.error()));
    }
    auto instance = variant.factory.construct(std::move(*settings), target);
    auto* planned = std::get_if<std::unique_ptr<Patch>>(&instance);
    if (planned == nullptr) {
        const auto* runtime_only = std::get_if<std::unique_ptr<RuntimeOnlyPatch>>(&instance);
        if (runtime_only == nullptr || *runtime_only == nullptr) {
            return std::unexpected("patch factory returned no patch instance");
        }
        return std::nullopt;
    }
    if (*planned == nullptr) {
        return std::unexpected("patch factory returned no patch instance");
    }

    PatchPlan plan(entry.id, target.image);
    (*planned)->build_plan(plan);
    auto dependencies = PlanVerification::critical_dependencies(plan);
    if (!dependencies.has_value()) {
        return std::unexpected(describe(dependencies.error()));
    }
    return PlanCandidate{std::move(instance), std::move(plan), std::move(*dependencies)};
}

[[nodiscard]] std::expected<void, std::string>
validate_original_dependencies(std::span<const std::byte> image, std::span<const CriticalDependency> dependencies) {
    for (const auto& dependency : dependencies) {
        if (!PlanVerification::matches(image, dependency)) {
            return std::unexpected(std::format("{}: target preimage does not match at RVA {:#010x}",
                                               dependency.operation, dependency.rva));
        }
    }
    return {};
}

[[nodiscard]] bool same_dependency_shape(const CriticalDependency& logical,
                                         const CriticalDependency& physical) noexcept {
    return logical.operation == physical.operation && logical.rva == physical.rva &&
           logical.expected.bytes.size() == physical.expected.bytes.size() &&
           logical.expected.mask == physical.expected.mask;
}

[[nodiscard]] std::expected<void, std::string>
validate_dependency_shapes(std::span<const CriticalDependency> logical, std::span<const CriticalDependency> physical) {
    if (logical.size() != physical.size()) {
        return std::unexpected("patch plan changed when mapped away from the PE preferred base");
    }

    for (std::size_t dependency_index = 0; dependency_index < logical.size(); ++dependency_index) {
        const auto& original = logical[dependency_index];
        const auto& relocated = physical[dependency_index];
        if (!same_dependency_shape(original, relocated)) {
            return std::unexpected(original.operation +
                                   ": patch plan changed when mapped away from the PE preferred base");
        }
    }
    return {};
}

[[nodiscard]] std::expected<void, std::string>
prove_dependency_rejection(const catalog::CatalogEntry& entry, const PatchVariant& variant, const TargetContext& target,
                           const SettingsCase& settings_case, std::span<std::byte> image,
                           const CriticalDependency& dependency) {
    const auto offset = static_cast<std::size_t>(dependency.rva) + dependency.byte_offset;
    if (offset >= image.size()) {
        return std::unexpected(dependency.operation + ": critical dependency lies outside the mapped image");
    }

    ByteMutation mutation(image[offset], dependency.mutation);
    auto candidate = build_candidate(entry, variant, target, settings_case);
    if (!candidate.has_value()) {
        return std::unexpected(dependency.operation + ": " + candidate.error());
    }
    if (!candidate->has_value()) {
        return std::unexpected(dependency.operation + ": rebuilt patch no longer has a plan");
    }

    auto prepared = PreparedPatchPlan::prepare(std::move((*candidate)->plan));
    if (prepared.has_value()) {
        return std::unexpected(dependency.operation + ": mutated dependency was accepted");
    }
    if (prepared.error().message != "target preimage does not match") {
        return std::unexpected(dependency.operation +
                               ": mutation was rejected for an unrelated reason: " + describe(prepared.error()));
    }
    return {};
}

[[nodiscard]] std::expected<void, std::string>
verify_relocated_plan(const catalog::CatalogEntry& entry, const PatchVariant& variant, const TargetContext& target,
                      const SettingsCase& settings_case, std::span<std::byte> image, PlanCandidate& candidate) {
    auto prepared = PreparedPatchPlan::prepare(std::move(candidate.plan));
    if (!prepared.has_value()) {
        return std::unexpected(describe(prepared.error()));
    }
    MutationReservations reservations;
    if (auto reserved = reservations.reserve(*prepared); !reserved.has_value()) {
        return std::unexpected(describe(reserved.error()));
    }

    for (const auto& dependency : candidate.dependencies) {
        if (auto rejection = prove_dependency_rejection(entry, variant, target, settings_case, image, dependency);
            !rejection.has_value()) {
            return std::unexpected(std::move(rejection.error()));
        }
    }
    return {};
}

[[nodiscard]] std::expected<std::optional<VerifiedPlan>, std::string>
verify_physical_variant(const catalog::CatalogEntry& entry, const PatchVariant& variant,
                        const TargetContext& physical_target, const SettingsCase& settings_case,
                        std::span<std::byte> image, std::span<const CriticalDependency> logical_dependencies) {
    auto physical_candidate = build_candidate(entry, variant, physical_target, settings_case);
    if (!physical_candidate.has_value()) {
        return std::unexpected(std::move(physical_candidate.error()));
    }
    if (!physical_candidate->has_value()) {
        return std::unexpected("patch no longer has a plan when mapped away from the PE preferred base");
    }

    const auto& physical_dependencies = (*physical_candidate)->dependencies;
    if (auto shapes = validate_dependency_shapes(logical_dependencies, physical_dependencies); !shapes.has_value()) {
        return std::unexpected(std::move(shapes.error()));
    }
    if (auto verified =
            verify_relocated_plan(entry, variant, physical_target, settings_case, image, **physical_candidate);
        !verified.has_value()) {
        return std::unexpected(std::move(verified.error()));
    }
    return VerifiedPlan{entry.id, variant.role, physical_dependencies.size()};
}

[[nodiscard]] std::expected<std::optional<VerifiedPlan>, std::string>
verify_settings_case(const catalog::CatalogEntry& entry, const PatchVariant& variant,
                     const TargetContext& physical_target, const SettingsCase& settings_case, MappedImage& image) {
    // Prove the untouched file at its preferred base before applying its native relocation table to the private copy.
    auto logical_target = physical_target;
    logical_target.image.base = image.preferred_base();
    auto logical_candidate = build_candidate(entry, variant, logical_target, settings_case);
    if (!logical_candidate.has_value()) {
        return std::unexpected(std::move(logical_candidate.error()));
    }
    if (!logical_candidate->has_value()) {
        return std::nullopt;
    }
    const auto& logical = (*logical_candidate)->dependencies;
    if (auto original = validate_original_dependencies(image.bytes(), logical); !original.has_value()) {
        return std::unexpected(std::move(original.error()));
    }

    if (auto relocated = image.relocate_to(image.base()); !relocated.has_value()) {
        return std::unexpected(std::move(relocated.error()));
    }
    auto verified = verify_physical_variant(entry, variant, physical_target, settings_case, image.bytes(), logical);
    if (auto restored = image.relocate_to(image.preferred_base()); !restored.has_value()) {
        return std::unexpected("could not restore the verifier image: " + restored.error());
    }
    return verified;
}

[[nodiscard]] std::expected<std::optional<VerifiedPlan>, std::string>
verify_variant(const catalog::CatalogEntry& entry, const PatchVariant& variant, const TargetContext& physical_target,
               MappedImage& image) {
    const auto& definition = catalog::variant_settings(entry.definition, variant);
    std::optional<VerifiedPlan> result;
    for (const auto& settings_case : verification_cases(definition)) {
        auto verified = verify_settings_case(entry, variant, physical_target, settings_case, image);
        if (!verified.has_value()) {
            return std::unexpected(describe(definition, settings_case) + ": " + verified.error());
        }
        if (verified->has_value() &&
            (!result.has_value() || (*verified)->critical_dependencies > result->critical_dependencies)) {
            result = **verified;
        }
    }
    return result;
}

} // namespace

std::expected<VerifiedImage, std::string> verify_supported_image(const std::filesystem::path& path, MappedImage& image,
                                                                 const catalog::Catalog& catalog) {
    const auto basename = path.filename().string();
    auto recognized = targets::recognize_mapped_image(basename, HostRole::Client, image.base(), image.bytes(),
                                                      targets::layouts::known_image_profiles());
    if (!recognized.has_value()) {
        return std::unexpected(std::string{recognized.error().detail});
    }

    VerifiedImage result{
        recognized->context.layout,
        recognized->context.image.identity,
        recognized->fingerprint,
        {},
    };

    for (const auto& entry : catalog.entries()) {
        for (const auto& variant : entry.definition.variants) {
            if (variant.layout != result.layout || variant.image != result.identity) {
                continue;
            }

            auto target = recognized->context;
            target.role = variant.role;
            auto verified = verify_variant(entry, variant, target, image);
            if (!verified.has_value()) {
                return std::unexpected(std::string{entry.id} + ": " + verified.error());
            }
            if (verified->has_value()) {
                result.plans.push_back(**verified);
            }
        }
    }
    return result;
}

} // namespace fusioncutter::verify
