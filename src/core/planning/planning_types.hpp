#pragma once

#include "../catalog/catalog_types.hpp"
#include "../config/configuration_types.hpp"

#include <FusionCutter/PluginApi.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace fc::planning {

inline constexpr std::size_t kPatchPlanByteCapacity = 2U * 1024U * 1024U;
inline constexpr std::size_t kValidationPlanByteCapacity = 64U * 1024U * 1024U;
inline constexpr std::size_t kHookObserverCapacity = 16;
inline constexpr std::uint32_t kHookStateByteCapacity = 1024;
inline constexpr std::uint32_t kHookStateAlignmentCapacity = 16;

// States are outcomes as well as phase gates; only WaitingForImage may resume after initial resolution.
enum class PatchState {
    Pending,
    Disabled,
    NotApplicable,
    WaitingForImage,
    Ready,
    Installed,
    Skipped,
    Failed,
};

// Phases identify the boundary that owns a failure and later determine which cleanup path is legal.
enum class PatchPhase {
    Selection,
    Settings,
    Create,
    Plan,
    Validation,
    Prepare,
    Commit,
    Activate,
};

// Failures deliberately retain shallow diagnostic identity rather than an exception or nested cause graph.
struct FailureReason {
    std::string message;
    std::optional<PatchPhase> phase;
    std::optional<std::string> operation;
    std::optional<std::string_view> related_patch;
    std::optional<std::string_view> related_group;
};

// The source view names the plugin catalog declaration from which this direct provider edge was resolved.
struct RequiredEdge {
    catalog::PatchIndex provider;
    std::string_view declared_source;
};

// Owns generic setting values until successful Create transfers their typed meaning into the plugin instance.
class ResolvedSettings {
  public:
    void push(config::ResolvedSettingValue value);
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::vector<FC_SettingValue> native_values() const;

  private:
    std::vector<config::ResolvedSettingValue> values_;
};

// Couples a successful Create handle with the exact callback table that must eventually destroy it.
class PatchInstance {
  public:
    PatchInstance() = default;
    PatchInstance(const FC_PatchCallbacks& callbacks, FC_PatchHandle handle) noexcept;
    PatchInstance(const PatchInstance&) = delete;
    PatchInstance& operator=(const PatchInstance&) = delete;
    PatchInstance(PatchInstance&& other) noexcept;
    PatchInstance& operator=(PatchInstance&& other) noexcept;
    ~PatchInstance();

    [[nodiscard]] FC_PatchHandle get() const noexcept;
    [[nodiscard]] const FC_PatchCallbacks& callbacks() const noexcept;
    [[nodiscard]] bool has_update() const noexcept;
    // Dispatches the installed callback through its retained context and handle on the serialized runtime pump.
    void update(FC_ReportToken report) noexcept;

  private:
    void reset() noexcept;

    const FC_PatchCallbacks* callbacks_{};
    FC_PatchHandle handle_{};
};

// These records own normalized inputs from the Plan callback so validation and installation can outlive it.
struct EvidenceRecord {
    FC_EvidenceKind kind{};
    std::vector<std::byte> bytes;
    std::vector<std::byte> mask;
    std::uint32_t target_rva{};
};

struct LocationRecord {
    FC_LocationKind kind{};
    std::uint32_t rva{};
    std::string name;
    std::string label;
    EvidenceRecord evidence;
};

struct NativeCallRecord {
    FC_NativeValue result{};
    FC_NativeStorage return_storage{};
    std::vector<FC_NativeArgument> arguments;
    FC_StackCleanup cleanup{};
    std::uint32_t stack_size{};
};

struct AddressTargetRecord {
    FC_AddressTargetKind kind{};
    std::uint32_t image_rva{};
    FC_DataHandle data{};
    std::uint64_t data_offset{};
    std::uintptr_t plugin_function{};
};

// Each operation payload retains only fields active for its validated ABI variant.
struct RequireOperation {
    LocationRecord location;
    std::uint64_t size{};
    std::uint32_t alignment{};
    bool writable{};
    std::optional<NativeCallRecord> native_call;
    std::uintptr_t resolved_address{};
};

struct WriteOperation {
    LocationRecord location;
    FC_WriteKind kind{};
    std::vector<std::byte> bytes;
    AddressTargetRecord target;
};

struct NopOperation {
    LocationRecord location;
    std::uint64_t size{};
};

struct RedirectOperation {
    LocationRecord location;
    FC_RedirectKind kind{};
    AddressTargetRecord target;
    std::uintptr_t original_target{};
};

struct DataAllocationOperation {
    FC_DataHandle handle{};
    std::uint64_t byte_size{};
    std::uint32_t alignment{};
    std::vector<std::byte> initial_bytes;
    std::string name;
};

struct InterfaceBindingOperation {
    std::string provider_patch;
    std::string id;
    std::uint32_t size{};
    void* context{};
    FC_InterfaceConnectFn connect{};
};

// Owner and observer forms share metadata for the physical site but retain their distinct callback contracts.
struct HookOperation {
    LocationRecord location;
    FC_HookKind kind{};
    std::uint32_t overwrite_size{};
    std::optional<NativeCallRecord> native_call;
    FC_HookBuilder builder{};
    bool observer{};
    void* context{};
    std::uintptr_t callback{};
    std::uintptr_t after{};
    void* original_context{};
    FC_BindOriginalFn bind_original{};
    std::uint32_t state_size{};
    std::uint32_t state_alignment{};
};

using OperationPayload = std::variant<RequireOperation, WriteOperation, NopOperation, RedirectOperation,
                                      DataAllocationOperation, InterfaceBindingOperation, HookOperation>;

struct OperationRecord {
    std::uint32_t index{};
    OperationPayload payload;
};

enum class ClaimAccess {
    Read,
    Write,
};

// Claims use half-open image-relative ranges and retain the plugin catalog owner for current and baseline records.
struct MemoryClaim {
    catalog::PatchIndex patch;
    FC_TargetImage image{};
    std::uint32_t rva{};
    std::uint64_t size{};
    ClaimAccess access{};
    std::uint32_t operation_index{};
};

// A SubmittedPlan owns all payload copied from callbacks and its image claims from before allocation.
struct SubmittedPlan {
    std::vector<OperationRecord> operations;
    std::vector<MemoryClaim> claims;
    std::size_t retained_bytes{};
};

// Mutable phase state lives here; immutable callbacks, policy, and schema remain in the indexed catalog patch.
struct PatchWorkRecord {
    catalog::PatchIndex patch;
    PatchState state;
    std::optional<FailureReason> reason;
    // These direct owners use their natural empty states instead of adding parallel presence flags.
    ResolvedSettings settings;
    std::vector<RequiredEdge> required_edges;
    SubmittedPlan plan;
    // Declared after plan so ordinary destruction of the work set still destroys plugin state first.
    PatchInstance instance;
};

// One record per patch in the plugin catalog is retained from resolution through installation outcome reporting.
class PatchWorkSet {
  public:
    explicit PatchWorkSet(const catalog::Catalog& catalog);

    [[nodiscard]] const catalog::Catalog& catalog() const noexcept;
    [[nodiscard]] std::span<PatchWorkRecord> records() noexcept;
    [[nodiscard]] std::span<const PatchWorkRecord> records() const noexcept;
    [[nodiscard]] PatchWorkRecord& record(catalog::PatchIndex patch) noexcept;
    [[nodiscard]] const PatchWorkRecord& record(catalog::PatchIndex patch) const noexcept;

  private:
    const catalog::Catalog* catalog_{};
    std::vector<PatchWorkRecord> records_;
};

// Projects one process-lifetime hook registry site into immutable input for validating additional participants.
struct InstalledHookSite {
    FC_TargetImage image{};
    std::uint32_t rva{};
    FC_HookKind kind{};
    std::optional<NativeCallRecord> native_call;
    std::uint32_t overwrite_size{};
    bool has_owner{};
    std::uint32_t observer_count{};
    std::uint32_t state_size{};
    // Borrows the preimage owned by the registry to validate late participants against logical original bytes.
    std::span<const std::byte> original_bytes;
};

// Baseline spans borrow process-lifetime installed state and retained failure state for one validation call.
struct ValidationBaseline {
    std::span<const MemoryClaim> installed_claims;
    std::span<const MemoryClaim> blocked_claims;
    std::span<const InstalledHookSite> installed_hook_sites;
};

// Identifies the patch operation that contributes an owner or observer to one shared physical hook site.
struct HookParticipant {
    catalog::PatchIndex patch;
    std::uint32_t operation_index{};
};

// An aggregate freezes one compatible physical site without allocating or choosing its physical builder.
struct HookAggregatePlan {
    FC_TargetImage image{};
    std::uint32_t rva{};
    FC_HookKind kind{};
    std::uint32_t overwrite_size{};
    std::optional<HookParticipant> owner;
    std::vector<HookParticipant> observers;
    std::uint32_t state_size{};
};

// The non-mutating result contains only surviving shared sites, ordered so each provider precedes its consumers.
struct InstallationPlan {
    std::vector<HookAggregatePlan> hook_aggregates;
    std::vector<catalog::PatchIndex> installation_order;
};

// Terminalizing an inactive patch always destroys plugin state before releasing framework-owned callback inputs.
void finish_inactive_patch(PatchWorkRecord& record, PatchState state, FailureReason reason);

} // namespace fc::planning
