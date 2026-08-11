#pragma once

#include "environment.hpp"
#include "outcome.hpp"
#include "patching.hpp"
#include "reporting.hpp"
#include "settings.hpp"
#include "target.hpp"

#include <atomic>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <typeindex>
#include <variant>

namespace fusioncutter {

template <typename T> class PatchInstanceSlot {
  public:
    void publish(T& instance) noexcept {
        instance_.store(&instance, std::memory_order_release);
    }

    [[nodiscard]] T* read() const noexcept {
        return instance_.load(std::memory_order_acquire);
    }

    void clear(T& instance) noexcept {
        auto* expected = &instance;
        static_cast<void>(
            instance_.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel, std::memory_order_acquire));
    }

  private:
    std::atomic<T*> instance_{};
};

class Patch {
  public:
    virtual ~Patch() = default;
    virtual void build_plan(PatchPlan& plan) = 0;
};

class RuntimePatch : public Patch {
  public:
    [[nodiscard]] virtual std::expected<void, OutcomeReason> prepare_runtime() {
        return {};
    }
    virtual void enable_runtime() noexcept {}
    virtual void disable_runtime() noexcept {}
};

class RuntimeOnlyPatch {
  public:
    virtual ~RuntimeOnlyPatch() = default;

    [[nodiscard]] virtual std::expected<void, OutcomeReason> prepare_runtime() {
        return {};
    }
    virtual void enable_runtime() noexcept {}
    virtual void disable_runtime() noexcept {}
};

class Updatable {
  public:
    virtual ~Updatable() = default;
    virtual void update() noexcept = 0;
};

using PatchInstance = std::variant<std::unique_ptr<Patch>, std::unique_ptr<RuntimeOnlyPatch>>;

struct PatchFactory {
    using Construct = PatchInstance (*)(ResolvedSettings&& settings, const TargetContext& target);

    std::type_index settings_type{typeid(NoSettings)};
    Construct construct{};
};

struct PresentationCategory {
    std::string_view name;
    int order;
};

enum class ImageTiming {
    Startup,
    OneShotLate,
};

enum class StartupFailurePolicy {
    Local,
    StartupRequired,
};

struct PatchVariant {
    TargetLayout layout;
    HostRole role;
    TargetImage image;
    ImageTiming image_timing{ImageTiming::Startup};
    StartupFailurePolicy failure_policy{StartupFailurePolicy::Local};
    PatchFactory factory;
    std::optional<SettingsDefinition> settings_override;
};

template <typename... Descriptors> class PatchVariants;

struct PatchDefinition {
    std::string_view name;
    bool enabled;
    bool configurable;
    PresentationCategory category;
    std::string_view description;
    SettingsDefinition settings;
    std::span<const PatchId> depends_on;
    std::span<const PatchId> includes;
    std::span<const PatchVariant> variants;
};

[[nodiscard]] inline const SettingsDefinition& settings_for_variant(const PatchDefinition& definition,
                                                                    const PatchVariant& variant) noexcept {
    return variant.settings_override.has_value() ? *variant.settings_override : definition.settings;
}

struct PatchBuildEnvelope {
    bool x86;
    bool x64;
    bool client;
    bool server;

    [[nodiscard]] constexpr bool supports(Architecture architecture) const noexcept {
        return architecture == Architecture::X86 ? x86 : x64;
    }

    [[nodiscard]] constexpr bool supports(HostRole role) const noexcept {
        return role == HostRole::Client ? client : server;
    }
};

} // namespace fusioncutter

#include "templates/patch.hpp"
