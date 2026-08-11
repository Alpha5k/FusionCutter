#include "plan_verification.hpp"

#include "validation.hpp"

#include <algorithm>
#include <cstddef>
#include <type_traits>
#include <variant>

namespace fusioncutter::patching_detail {

std::expected<std::vector<CriticalDependency>, OutcomeReason>
PlanVerification::critical_dependencies(const PatchPlan& plan) {
    if (!plan.storage_) {
        return std::unexpected(operation_failure("patch plan has already been consumed"));
    }

    std::vector<CriticalDependency> dependencies;
    dependencies.reserve(plan.storage_->operations.size());

    for (const auto& operation : plan.storage_->operations) {
        auto dependency = std::visit(
            [&](const auto& spec) -> std::expected<void, OutcomeReason> {
                using Spec = std::remove_cvref_t<decltype(spec)>;
                if constexpr (std::same_as<Spec, AllocationSpec>) {
                    return {};
                } else {
                    if (!valid_pattern(spec.expected)) {
                        return std::unexpected(
                            operation_failure("critical dependency cannot be mutation-tested", operation.name));
                    }

                    std::size_t offset{};
                    std::byte mutation{1};
                    if (!spec.expected.mask.empty()) {
                        const auto constrained = std::ranges::find_if(spec.expected.mask, [](std::byte mask) {
                            return mask != std::byte{};
                        });
                        offset = static_cast<std::size_t>(constrained - spec.expected.mask.begin());
                        mutation = *constrained;
                    }
                    dependencies.push_back({operation.name, spec.rva, offset, mutation, spec.expected});
                    return {};
                }
            },
            operation.spec);
        if (!dependency.has_value()) {
            return std::unexpected(std::move(dependency.error()));
        }
    }
    return dependencies;
}

bool PlanVerification::matches(std::span<const std::byte> image, const CriticalDependency& dependency) noexcept {
    const auto offset = static_cast<std::size_t>(dependency.rva);
    if (offset > image.size() || dependency.expected.bytes.size() > image.size() - offset) {
        return false;
    }
    return pattern_matches(reinterpret_cast<std::uintptr_t>(image.data() + offset), dependency.expected);
}

} // namespace fusioncutter::patching_detail
