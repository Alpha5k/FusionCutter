#pragma once

#include "../catalog/catalog_types.hpp"
#include "../patching/patch_transaction.hpp"
#include "../planning/planning_types.hpp"

#include <atomic>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <filesystem>
#include <span>
#include <string_view>

namespace fc::patching {
class HookPreparation;
class HookRegistry;
struct HookResourceView;
} // namespace fc::patching

namespace fc::targets {
class RecognizedTarget;
} // namespace fc::targets

struct _EXCEPTION_POINTERS;

namespace fc::runtime {

struct CrashWorkspace;

// Framework phases are separate from immutable annotations so a fault sees the current serialized operation.
enum class CorePhase : std::uint32_t {
    Idle,
    Startup,
    TargetRecognition,
    PluginAdmission,
    Configuration,
    Validation,
    Installation,
    Running,
};

// Annotation kinds distinguish durable native ownership from contextual identities in the eventual crash report.
enum class CrashAnnotationKind : std::uint32_t {
    Plugin,
    Callback,
    Patch,
    Mutation,
    Hook,
    NativeResource,
    RetainedFailure,
};

// Fixed-size copied records let an exceptional reader traverse only immutable framework-owned memory. Label identifies
// the owning plugin or patch, detail names the version or resource role, and module identifies physical code origin.
struct CrashAnnotation {
    CrashAnnotationKind kind{};
    std::uint32_t patch{};
    std::uintptr_t address{};
    std::uint64_t size{};
    std::uint32_t timestamp{};
    char label[96]{};
    char detail[96]{};
    char module[96]{};
};

// CoreRuntime owns these lock-free lifecycle cursors while CrashReporter supplies their exceptional read view.
struct CrashPhaseCursors {
    std::atomic<std::uint32_t> core_phase{static_cast<std::uint32_t>(CorePhase::Idle)};
    std::atomic<std::uint32_t> current_patch{std::numeric_limits<std::uint32_t>::max()};
    std::atomic<std::uint32_t> current_patch_phase{static_cast<std::uint32_t>(planning::PatchPhase::Selection)};
};

// A snapshot borrows the immutable published prefix and samples the live serialized lifecycle cursors.
struct CrashSnapshotView {
    std::span<const CrashAnnotation> annotations;
    std::uint32_t omitted{};
    CorePhase core_phase{};
    catalog::PatchIndex current_patch{};
    planning::PatchPhase current_patch_phase{};
    bool has_current_patch{};
};

// Publishes a bounded append-only ownership snapshot without exposing mutable runtime containers to a fault handler.
class CrashReporter final {
  public:
    explicit CrashReporter(CrashPhaseCursors& cursors) noexcept;
    CrashReporter(const CrashReporter&) = delete;
    CrashReporter& operator=(const CrashReporter&) = delete;
    ~CrashReporter();

    // Reserves the fixed snapshot storage before any provisional or admitted native callback can execute.
    [[nodiscard]] bool install(const std::filesystem::path& output_directory = {}) noexcept;

    // Copy loader facts before handler installation so reports for unsupported targets still identify their host.
    void set_session(FC_HostRole role, std::string_view selected_proxy_basename) noexcept;

    // Serialized lifecycle code advances the live cursors around work that a simultaneous fault must attribute.
    void set_core_phase(CorePhase phase) noexcept;
    void set_current_patch(catalog::PatchIndex patch, planning::PatchPhase phase) noexcept;
    void set_patch_phase(planning::PatchPhase phase) noexcept;
    void clear_current_patch() noexcept;
    // Target recognition publishes a fixed tuple so the handler never traverses live target ownership.
    void set_target(const targets::RecognizedTarget& target) noexcept;

    // Final admission copies retained module and callback ownership before any patch installation starts.
    void publish_catalog(const catalog::Catalog& catalog) noexcept;
    // Successful publication consumes the last durable view of the patch plan after native owners transfer.
    void publish_installed_patch(catalog::PatchIndex patch, const catalog::Catalog& catalog,
                                 const targets::RecognizedTarget& target, const planning::SubmittedPlan& plan,
                                 const patching::NativePatchResources& resources,
                                 const patching::HookRegistry& hooks) noexcept;
    // Exposed failure publication records blockers and every pinned native resource after retained ownership transfers.
    void publish_retained_failure(catalog::PatchIndex patch, patching::RollbackResult rollback,
                                  std::span<const planning::MemoryClaim> blocked_claims, std::string_view patch_id,
                                  const targets::RecognizedTarget& target,
                                  const patching::NativePatchResources& resources,
                                  const patching::HookPreparation& hooks) noexcept;

    [[nodiscard]] CrashSnapshotView snapshot() const noexcept;

    // Expected faults are thread-local and lexically scoped so another thread's real fault remains observable.
    class ExpectedFaultScope final {
      public:
        explicit ExpectedFaultScope(std::uint32_t exception_code) noexcept;
        ExpectedFaultScope(const ExpectedFaultScope&) = delete;
        ExpectedFaultScope& operator=(const ExpectedFaultScope&) = delete;
        ~ExpectedFaultScope();

      private:
        std::uint32_t previous_{};
    };

    [[nodiscard]] static bool expected_fault(std::uint32_t exception_code) noexcept;

  private:
    // The serialized writer completes a fixed record before extending the exception handler's prefix atomically.
    void append(CrashAnnotationKind kind, std::uint32_t patch, std::uintptr_t address, std::uint64_t size,
                std::string_view label, std::string_view detail = {}, std::string_view module = {},
                std::uint32_t timestamp = 0) noexcept;
    // These helpers lower durable framework allocation owners into fixed records without allocating.
    void publish_native_resources(catalog::PatchIndex patch, std::string_view patch_id,
                                  const patching::NativePatchResources& resources) noexcept;
    void publish_hook_resource(catalog::PatchIndex patch, std::string_view patch_id,
                               const patching::HookResourceView& resource) noexcept;
    static void publish_hook_resource_callback(void* context, const patching::HookResourceView& resource) noexcept;
    static long __stdcall vectored_handler(_EXCEPTION_POINTERS* exception) noexcept;
    [[nodiscard]] long capture(_EXCEPTION_POINTERS* exception) noexcept;

    static constexpr std::size_t kAnnotationByteCapacity = 512U * 1024U;
    static constexpr std::size_t kAnnotationCapacity = kAnnotationByteCapacity / sizeof(CrashAnnotation);
    CrashAnnotation* annotations_{};
    std::atomic<std::uint32_t> published_count_{};
    std::atomic<std::uint32_t> omitted_count_{};
    CrashPhaseCursors* cursors_{};
    void* handler_{};
    std::array<wchar_t, 32'768> report_path_{};
    CrashWorkspace* workspace_{};
    std::atomic<std::uint32_t> report_attempts_{};
    std::atomic_bool file_started_{};
    std::atomic_flag handling_ = ATOMIC_FLAG_INIT;
    std::uint64_t process_started_{};
    FC_TargetLayout target_layout_{};
    FC_HostRole target_role_{};
    FC_Architecture target_architecture_{};
    std::array<char, 96> target_profile_{};
    std::array<char, 96> executable_basename_{};
    std::array<char, 96> selected_proxy_basename_{};
    std::uint64_t process_creation_time_{};
    std::uint32_t windows_major_{};
    std::uint32_t windows_minor_{};
    std::uint32_t windows_build_{};
    FC_HostRole session_role_{};
    std::atomic_bool target_ready_{};

    static std::atomic<CrashReporter*> active_;
};

} // namespace fc::runtime
