#pragma once

#include "catalog/catalog.hpp"
#include "config/configuration.hpp"

#include <FusionCutter/outcome.hpp>

#include <expected>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

namespace fusioncutter {

using LateImageProbe = std::expected<std::optional<TargetContext>, OutcomeReason> (*)(TargetLayout layout,
                                                                                      HostRole role, TargetImage image);

struct ActiveStatusContributor {
    std::string_view name;
    const StatusContributor* contributor;
};

class StartupState {
  public:
    StartupState(const StartupState&) = delete;
    StartupState(StartupState&&) noexcept;
    StartupState& operator=(const StartupState&) = delete;
    StartupState& operator=(StartupState&&) noexcept;
    ~StartupState();

    [[nodiscard]] const InitializationResult& initialization_result() const noexcept;
    [[nodiscard]] std::span<const PatchResult> patch_results() const noexcept;
    [[nodiscard]] std::span<const ActiveStatusContributor> status_contributors() const noexcept;
    [[nodiscard]] std::uint64_t status_revision() const noexcept;

    void update(LateImageProbe late_image_probe) noexcept;

  private:
    class Impl;

    explicit StartupState(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;

    friend StartupState run_startup(catalog::Catalog catalog, config::Configuration configuration,
                                    std::span<const TargetContext> startup_images);
};

[[nodiscard]] StartupState run_startup(catalog::Catalog catalog, config::Configuration configuration,
                                       std::span<const TargetContext> startup_images);

} // namespace fusioncutter
