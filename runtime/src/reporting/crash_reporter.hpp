#pragma once

#include "status.hpp"

#include <fc/abi.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

struct _EXCEPTION_POINTERS;
struct _EXCEPTION_RECORD;

namespace fc::runtime {

struct PatchRecord;
struct CrashWorkspace;
class ReportWriter;

enum class CrashAnnotationKind : std::uint32_t {
    Plugin,
    Patch,
    Mutation,
    Hook,
    NativeResource,
    Module
};

// Supplies a copied identity or address range for attribution in crash reports.
struct CrashAnnotation {
    CrashAnnotationKind kind{};
    // Compared as an identity against copied patch records; crash reporting never dereferences it.
    const PatchRecord *owner{};
    std::uintptr_t address{};
    // Byte extent from address; zero describes an exact address instead of a range.
    std::uint64_t size{};
    // PE timestamp for a recorded module.
    std::uint32_t timestamp{};
    std::string label, detail, module;
};

struct CrashAnnotationBatch;

// Observes native exceptions and records fatal-error reports using preallocated storage and copied runtime facts.
class CrashReporter final {
  public:
    CrashReporter() noexcept = default;
    CrashReporter(const CrashReporter &) = delete;
    CrashReporter &operator=(const CrashReporter &) = delete;
    // Removes observation and frees retained batches; the caller must ensure publication and captures have stopped.
    ~CrashReporter();

    // Installs first-chance observation once, before patch callbacks; only one installed reporter may be alive.
    // Copies facts and preallocates storage; false with a Windows error must stop runtime startup.
    [[nodiscard]] bool install(const std::filesystem::path &directory, const reporting::SessionFacts &facts) noexcept;

    // The control thread copies the recognized profile once, before its patches install native operations.
    void set_target(std::string_view profile);

    // The control thread copies records before associated callbacks or resources are exposed; source storage may then be reused.
    // Published batches stay immutable and alive until destruction, so capture readers need no publication lock.
    void publish(std::span<const CrashAnnotation> annotations);

    // Records a bounded fatal reason independently of logging; reason need only live for this call and owner is an identity.
    // Capture can fail or exhaust its attempts; the caller must still terminate.
    void capture_fatal(std::string_view reason, std::uint32_t windows_error = 0, const PatchRecord *owner = nullptr) noexcept;

    // Runs callback synchronously; exact access-violation instruction and read/write/execute kind match nested thread-local scopes.
    // Returns 0 invalid, 1 completed, 2 caught; other faults search normally. Callback/context must live through the call.
    static std::uint32_t invoke_expected_fault(std::uintptr_t instruction, std::uint32_t access_kind, FC_ExpectedFaultCallback callback,
                                               void *context) noexcept;
    // Checks access-violation instruction and access kind against any active nested expectation on this thread.
    static bool expected_fault(const _EXCEPTION_RECORD &record) noexcept;

  private:
    // Dispatches first-chance observation to the active reporter, leaving exception handling to Windows.
    static long __stdcall vectored_handler(_EXCEPTION_POINTERS *) noexcept;
    // Reports qualifying native exceptions when a workspace is available; always returns EXCEPTION_CONTINUE_SEARCH.
    long capture(_EXCEPTION_POINTERS *) noexcept;
    // Tries once to claim handling_ and a report attempt; true returns its zero-based index with the flag still held.
    // The caller must clear handling_ after reporting; false leaves another capture's ownership unchanged.
    [[nodiscard]] bool begin_report(std::uint32_t &attempt) noexcept;
    // Writes and flushes size bytes from the workspace, or emits an I/O diagnostic; caller must hold handling_.
    void write_report(std::size_t size, std::uint32_t code, std::uintptr_t address) noexcept;
    // Appends build, process, thread and session identity, acquiring the recognized profile when available.
    void append_session(ReportWriter &report) const noexcept;

    std::atomic<CrashAnnotationBatch *> annotations_{};
    void *handler_{};
    std::wstring report_path_;
    CrashWorkspace *workspace_{};
    std::uint32_t report_attempts_{};
    bool file_started_{};
    // Gives one faulting thread exclusive use of the workspace and report attempt counter.
    std::atomic_flag handling_ = ATOMIC_FLAG_INIT;
    std::string target_profile_;
    reporting::SessionFacts session_;
    std::atomic_bool target_ready_{};
    static std::atomic<CrashReporter *> active_;
};

} // namespace fc::runtime
