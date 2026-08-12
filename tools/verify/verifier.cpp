#include "verifier.hpp"

#include "patching/patching.hpp"
#include "patching/plan_verification.hpp"
#include "targets/layouts/profiles.hpp"
#include "targets/recognition.hpp"

#include <FusionCutter/patch.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
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

[[nodiscard]] std::expected<std::optional<PlanCandidate>, std::string>
build_candidate(const catalog::CatalogEntry& entry, const PatchVariant& variant, const TargetContext& target) {
    auto instance =
        variant.factory.construct(catalog::variant_settings(entry.definition, variant).make_defaults(), target);
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
            return std::unexpected(dependency.operation + ": target preimage does not match");
        }
    }
    return {};
}

[[nodiscard]] bool same_dependency(const CriticalDependency& logical, const CriticalDependency& physical) noexcept {
    return logical.operation == physical.operation && logical.rva == physical.rva &&
           logical.expected.bytes.size() == physical.expected.bytes.size() &&
           logical.expected.mask == physical.expected.mask;
}

[[nodiscard]] std::expected<void, std::string> relocate_dependencies(std::span<std::byte> image,
                                                                     std::span<const CriticalDependency> logical,
                                                                     std::span<const CriticalDependency> physical) {
    if (logical.size() != physical.size()) {
        return std::unexpected("patch plan changed when mapped away from the PE preferred base");
    }

    for (std::size_t dependency_index = 0; dependency_index < logical.size(); ++dependency_index) {
        const auto& original = logical[dependency_index];
        const auto& relocated = physical[dependency_index];
        if (!same_dependency(original, relocated)) {
            return std::unexpected(original.operation +
                                   ": patch plan changed when mapped away from the PE preferred base");
        }

        const auto offset = static_cast<std::size_t>(relocated.rva);
        if (offset > image.size() || relocated.expected.bytes.size() > image.size() - offset) {
            return std::unexpected(relocated.operation + ": target preimage lies outside the mapped image");
        }
    }

    for (const auto& relocated : physical) {
        const auto offset = static_cast<std::size_t>(relocated.rva);
        for (std::size_t byte_index = 0; byte_index < relocated.expected.bytes.size(); ++byte_index) {
            const auto mask = relocated.expected.mask.empty() ? std::byte{0xFF} : relocated.expected.mask[byte_index];
            auto& target = image[offset + byte_index];
            target = (target & ~mask) | (relocated.expected.bytes[byte_index] & mask);
        }
    }
    return {};
}

[[nodiscard]] std::expected<void, std::string>
prove_dependency_rejection(const catalog::CatalogEntry& entry, const PatchVariant& variant, const TargetContext& target,
                           std::span<std::byte> image, const CriticalDependency& dependency) {
    const auto offset = static_cast<std::size_t>(dependency.rva) + dependency.byte_offset;
    if (offset >= image.size()) {
        return std::unexpected(dependency.operation + ": critical dependency lies outside the mapped image");
    }

    ByteMutation mutation(image[offset], dependency.mutation);
    auto candidate = build_candidate(entry, variant, target);
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
                      std::span<std::byte> image, PlanCandidate& candidate) {
    auto prepared = PreparedPatchPlan::prepare(std::move(candidate.plan));
    if (!prepared.has_value()) {
        return std::unexpected(describe(prepared.error()));
    }
    MutationReservations reservations;
    if (auto reserved = reservations.reserve(*prepared); !reserved.has_value()) {
        return std::unexpected(describe(reserved.error()));
    }

    for (const auto& dependency : candidate.dependencies) {
        if (auto rejection = prove_dependency_rejection(entry, variant, target, image, dependency);
            !rejection.has_value()) {
            return std::unexpected(std::move(rejection.error()));
        }
    }
    return {};
}

[[nodiscard]] std::expected<std::optional<VerifiedPlan>, std::string>
verify_variant(const catalog::CatalogEntry& entry, const PatchVariant& variant, const TargetContext& physical_target,
               std::uintptr_t preferred_base, std::span<std::byte> image) {
    // Prove the untouched file at its preferred base before adapting base-dependent proofs to the private copy.
    auto logical_target = physical_target;
    logical_target.image.base = preferred_base;
    auto logical_candidate = build_candidate(entry, variant, logical_target);
    if (!logical_candidate.has_value()) {
        return std::unexpected(std::move(logical_candidate.error()));
    }
    if (!logical_candidate->has_value()) {
        return std::nullopt;
    }
    const auto& logical = (*logical_candidate)->dependencies;
    if (auto original = validate_original_dependencies(image, logical); !original.has_value()) {
        return std::unexpected(std::move(original.error()));
    }

    auto physical_candidate = build_candidate(entry, variant, physical_target);
    if (!physical_candidate.has_value()) {
        return std::unexpected(std::move(physical_candidate.error()));
    }
    if (!physical_candidate->has_value()) {
        return std::unexpected("patch no longer has a plan when mapped away from the PE preferred base");
    }
    const auto& physical = (*physical_candidate)->dependencies;
    if (auto relocated = relocate_dependencies(image, logical, physical); !relocated.has_value()) {
        return std::unexpected(std::move(relocated.error()));
    }
    auto verified = verify_relocated_plan(entry, variant, physical_target, image, **physical_candidate);
    if (auto restored = relocate_dependencies(image, physical, logical); !restored.has_value()) {
        return std::unexpected("could not restore the verifier image: " + restored.error());
    }
    if (!verified.has_value()) {
        return std::unexpected(std::move(verified.error()));
    }
    return VerifiedPlan{entry.id, variant.role, physical.size()};
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
            auto verified = verify_variant(entry, variant, target, image.preferred_base(), image.bytes());
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
