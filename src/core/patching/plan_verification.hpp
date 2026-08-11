#pragma once

#include "plan_storage.hpp"

#include <FusionCutter/outcome.hpp>
#include <FusionCutter/patching.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

namespace fusioncutter::patching_detail {

struct CriticalDependency {
    std::string operation;
    std::uint32_t rva;
    std::size_t byte_offset;
    std::byte mutation;
    OwnedPattern expected;
};

class PlanVerification {
  public:
    [[nodiscard]] static std::expected<std::vector<CriticalDependency>, OutcomeReason>
    critical_dependencies(const PatchPlan& plan);
    [[nodiscard]] static bool matches(std::span<const std::byte> image, const CriticalDependency& dependency) noexcept;
};

} // namespace fusioncutter::patching_detail
