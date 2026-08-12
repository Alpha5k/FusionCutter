#pragma once

#include "../patch.hpp"

#include <array>
#include <concepts>
#include <cstddef>
#include <optional>
#include <utility>

namespace fusioncutter {
namespace patch_detail {

static_assert(sizeof(void*) == 4 || sizeof(void*) == 8);
inline constexpr auto current_architecture = sizeof(void*) == 4 ? Architecture::X86 : Architecture::X64;

template <typename PatchType, typename... Arguments>
[[nodiscard]] PatchInstance make_patch_instance(Arguments&&... arguments) {
    if constexpr (std::derived_from<PatchType, Patch>) {
        return std::unique_ptr<Patch>(std::make_unique<PatchType>(std::forward<Arguments>(arguments)...));
    } else {
        return std::unique_ptr<RuntimeOnlyPatch>(std::make_unique<PatchType>(std::forward<Arguments>(arguments)...));
    }
}

template <typename PatchType, typename Settings>
[[nodiscard]] PatchInstance construct_patch(ResolvedSettings&& resolved, const TargetContext& target) {
    if constexpr (std::same_as<Settings, NoSettings>) {
        static_cast<void>(std::move(resolved).template take<NoSettings>());
        return make_patch_instance<PatchType>(target);
    } else {
        auto settings = std::move(resolved).template take<Settings>();
        return make_patch_instance<PatchType>(std::move(settings), target);
    }
}

template <typename PatchType, typename Settings = NoSettings>
    requires(std::derived_from<PatchType, Patch> || std::derived_from<PatchType, RuntimeOnlyPatch>)
[[nodiscard]] PatchFactory patch_factory() noexcept {
    if constexpr (std::same_as<Settings, NoSettings>) {
        static_assert(std::constructible_from<PatchType, const TargetContext&>);
    } else {
        static_assert(std::constructible_from<PatchType, Settings, const TargetContext&>);
    }
    return {typeid(Settings), &patch_detail::construct_patch<PatchType, Settings>};
}

template <typename PatchType, TargetLayout Layout, typename Settings> struct PatchVariantDescriptor {
    static constexpr auto layout = Layout;

    HostRole role;
    TargetImage image;
    ImageTiming image_timing;
    StartupFailurePolicy failure_policy;
    std::optional<SettingsDefinition> settings_override;

    [[nodiscard]] PatchVariant materialize() const noexcept {
        return {
            .layout = Layout,
            .role = role,
            .image = image,
            .image_timing = image_timing,
            .failure_policy = failure_policy,
            .factory = patch_factory<PatchType, Settings>(),
            .settings_override = settings_override,
        };
    }
};

template <typename Descriptor>
inline constexpr bool compatible_with_current_build = target_architecture(Descriptor::layout) == current_architecture;

template <std::size_t Size, typename Descriptor>
void append_compatible_variant(std::array<PatchVariant, Size>& variants, std::size_t& next,
                               const Descriptor& descriptor) noexcept {
    if constexpr (compatible_with_current_build<Descriptor>) {
        variants[next++] = descriptor.materialize();
    }
}

} // namespace patch_detail

template <typename PatchType, TargetLayout Layout, typename Settings = NoSettings>
    requires(std::derived_from<PatchType, Patch> || std::derived_from<PatchType, RuntimeOnlyPatch>)
[[nodiscard]] auto make_patch_variant(HostRole role, TargetImage image, ImageTiming image_timing = ImageTiming::Startup,
                                      StartupFailurePolicy failure_policy = StartupFailurePolicy::Local) noexcept {
    return patch_detail::PatchVariantDescriptor<PatchType, Layout, Settings>{role, image, image_timing, failure_policy};
}

template <typename PatchType, TargetLayout Layout, typename Settings>
    requires(std::derived_from<PatchType, Patch> || std::derived_from<PatchType, RuntimeOnlyPatch>)
[[nodiscard]] auto make_patch_variant(HostRole role, TargetImage image, SettingsDefinition settings_override,
                                      ImageTiming image_timing = ImageTiming::Startup,
                                      StartupFailurePolicy failure_policy = StartupFailurePolicy::Local) noexcept {
    return patch_detail::PatchVariantDescriptor<PatchType, Layout, Settings>{role, image, image_timing, failure_policy,
                                                                             std::move(settings_override)};
}

template <typename... Descriptors> class PatchVariants {
  public:
    explicit PatchVariants(Descriptors... descriptors) noexcept {
        std::size_t next = 0;
        (patch_detail::append_compatible_variant(variants_, next, descriptors), ...);
    }

    [[nodiscard]] operator std::span<const PatchVariant>() const noexcept {
        return variants_;
    }

  private:
    static constexpr std::size_t kSize =
        (std::size_t{0} + ... + static_cast<std::size_t>(patch_detail::compatible_with_current_build<Descriptors>));
    std::array<PatchVariant, kSize> variants_{};
};

template <typename... Descriptors> PatchVariants(Descriptors...) -> PatchVariants<Descriptors...>;

} // namespace fusioncutter
