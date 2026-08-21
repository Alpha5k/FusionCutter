#pragma once

#include <FusionCutter/SDK.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace fc::test {

// Scenario stops after common validation, so its states exclude the runtime Pending and Installed states.
enum class PatchState {
    Disabled,
    NotApplicable,
    WaitingForImage,
    Ready,
    Skipped,
    Failed,
};

// PatchPhase identifies the common lifecycle boundary that produced a validation failure.
enum class PatchPhase {
    Selection,
    Settings,
    Create,
    Plan,
    Validation,
};

// OperationKind exposes mutation intent without leaking private transaction or callback representations.
enum class OperationKind {
    Require,
    Write,
    Nop,
    Redirect,
    AllocateData,
    BindInterface,
    Hook,
    Observe,
};

// ClaimAccess distinguishes evidence reads from exclusive mutation ranges in the validated plan.
enum class ClaimAccess {
    Read,
    Write,
};

// Operation is a shallow assertion surface; the framework retains private payloads and native callback addresses.
struct Operation {
    std::uint32_t operation_index;
    OperationKind kind;
    std::optional<fc::TargetImage> image;
    std::optional<fc::Rva> rva;
    std::uint64_t byte_size;
    bool has_evidence;
};

// Claim identifies the validated image range and the operation responsible for that dependency or mutation.
struct Claim {
    fc::TargetImage image;
    fc::Rva rva;
    std::uint64_t byte_size;
    ClaimAccess access;
    std::uint32_t operation_index;
};

// Rejected candidates retain their path and safely copied ID without consulting the admitted plugin catalog.
struct PluginResult {
    std::optional<std::filesystem::path> path;
    std::optional<std::string> id;
    bool admitted;
    std::optional<std::string> reason;
};

// PatchResult owns the terminal validation outcome and a shallow, assertion-oriented copy of any submitted plan.
struct PatchResult {
    std::string id;
    PatchState state;
    std::optional<std::string> reason;
    std::optional<PatchPhase> phase;
    std::optional<std::string> operation;
    std::optional<std::string> related_patch;
    std::optional<std::string> related_group;
    std::vector<Operation> operations;
    std::vector<Claim> claims;
};

// Results directly own every copied assertion value, so they remain valid after plugin DLL and temporary cleanup.
class ScenarioResult {
  public:
    // Returned spans borrow immutable storage owned by this result object.
    [[nodiscard]] std::span<const PluginResult> plugins() const noexcept;
    [[nodiscard]] std::span<const PatchResult> patches() const noexcept;
    [[nodiscard]] const PluginResult* find_plugin(std::string_view id) const noexcept;
    [[nodiscard]] const PatchResult* find_patch(std::string_view id) const noexcept;

  private:
    std::vector<PluginResult> plugins_;
    std::vector<PatchResult> patches_;

    friend class Scenario;
};

// Scenario supplies explicit inputs to the production admission, configuration, resolution, planning, and validation
// used by production; it never prepares or commits native mutation.
class Scenario {
  public:
    Scenario(fc::TargetLayout layout, fc::HostRole role);

    // Adds an explicit external DLL candidate; discovery directories are intentionally not consulted.
    void add_plugin(std::filesystem::path path);

    // Maps an ordinary PE file into a private copy of the loaded image; the source file remains read-only.
    void add_image(fc::TargetImage image, std::string image_profile, std::filesystem::path path);

    // The supplied bytes for the loaded image remain borrowed until validate() returns.
    void add_image(fc::TargetImage image, std::string image_profile, std::span<const std::byte> bytes);

    // INIs are copied into the scenario workspace before ordinary generation or parsing can write anything.
    void use_config(std::filesystem::path directory);

    // Runs common admission through plan validation and returns before any native preparation or mutation.
    [[nodiscard]] std::expected<ScenarioResult, fc::Error> validate() const;

  private:
    struct ImageInput {
        fc::TargetImage image;
        std::string image_profile;
        std::variant<std::filesystem::path, std::span<const std::byte>> source;
    };

    fc::TargetLayout layout_;
    fc::HostRole role_;
    std::vector<std::filesystem::path> plugins_;
    std::vector<ImageInput> images_;
    std::optional<std::filesystem::path> config_directory_;
};

} // namespace fc::test
