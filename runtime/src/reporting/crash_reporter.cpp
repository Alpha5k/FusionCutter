#include "crash_reporter.hpp"

#include "crash_report.hpp"

#include <memory>

namespace fc::runtime {

using namespace crash_detail;

void crash_detail::write_fallback(std::string_view text) noexcept {
    std::array<wchar_t, 1024> wide{};
    const auto characters =
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), static_cast<int>(wide.size() - 1));
    OutputDebugStringW(wide.data());
    const auto console = GetStdHandle(STD_ERROR_HANDLE);
    DWORD mode{}, written{};
    if (console && console != INVALID_HANDLE_VALUE && GetConsoleMode(console, &mode)) {
        WriteConsoleW(console, wide.data(), static_cast<DWORD>(characters), &written, nullptr);
    }
}

std::atomic<CrashReporter *> CrashReporter::active_{};

CrashReporter::~CrashReporter() {
    // Unregister observation before freeing storage; the caller has already stopped captures and publication.
    if (handler_ != nullptr) {
        RemoveVectoredExceptionHandler(handler_);
        handler_ = nullptr;
    }

    auto *expected = this;
    active_.compare_exchange_strong(expected, nullptr, std::memory_order_release, std::memory_order_relaxed);

    auto *batch = annotations_.load(std::memory_order_relaxed);
    while (batch) {
        auto *next = batch->next;
        delete batch;
        batch = next;
    }

    delete workspace_;
}

bool CrashReporter::install(const std::filesystem::path &directory, const reporting::SessionFacts &facts) noexcept {
    try {
        // Prepare the output path, session facts and workspace before the exception handler can run.
        session_ = facts;
        report_path_ = (directory / "FusionCutter.crash.log").native();
        auto workspace = std::make_unique<CrashWorkspace>();

        // Copy this runtime's module identity before its observer becomes reachable by faulting threads.
        CrashModuleRecord runtime{};
        if (resolve_module(reinterpret_cast<std::uintptr_t>(&CrashReporter::vectored_handler), runtime)) {
            const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(runtime.base);
            const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(runtime.base + dos->e_lfanew);
            CrashAnnotation annotation{.kind = CrashAnnotationKind::Module,
                                       .address = runtime.base,
                                       .size = runtime.size,
                                       .timestamp = nt->FileHeader.TimeDateStamp,
                                       .label = "Fusion Cutter runtime",
                                       .module = runtime.basename.data()};
            publish({&annotation, 1});
        }

        // Publish initialized storage before Windows can dispatch to the newly registered handler.
        workspace_ = workspace.release();
        active_.store(this, std::memory_order_release);
        handler_ = AddVectoredExceptionHandler(1, &CrashReporter::vectored_handler);
        if (!handler_) {
            // Undo active publication and workspace ownership when observer registration fails.
            active_.store(nullptr, std::memory_order_release);
            delete workspace_;
            workspace_ = nullptr;
            return false;
        }
        return true;
    } catch (...) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return false;
    }
}

void CrashReporter::set_target(std::string_view profile) {
    target_profile_ = profile;
    // Captures acquire this flag before reading the profile copied by the control thread.
    target_ready_.store(true, std::memory_order_release);
}

void CrashReporter::publish(std::span<const CrashAnnotation> records) {
    auto batch = std::make_unique<CrashAnnotationBatch>();
    batch->records.assign(records.begin(), records.end());
    batch->next = annotations_.load(std::memory_order_relaxed);

    // Only the control thread publishes; faulting threads read complete immutable batches.
    annotations_.store(batch.release(), std::memory_order_release);
}

bool CrashReporter::begin_report(std::uint32_t &attempt) noexcept {
    // Acquire without waiting: a recursive or overlapping capture must not block on the faulting thread.
    if (handling_.test_and_set(std::memory_order_acq_rel)) {
        return false;
    }

    attempt = report_attempts_;
    if (attempt >= kMaximumCrashAttempts || !workspace_) {
        handling_.clear(std::memory_order_release);
        return false;
    }
    // Count attempts before file I/O so repeated output failures still consume the report budget.
    ++report_attempts_;
    return true;
}

void CrashReporter::write_report(std::size_t size, std::uint32_t code, std::uintptr_t address) noexcept {
    // Replace stale output until a full write and flush succeeds; later reports append to that process's output.
    const bool append = file_started_;
    const auto file =
        CreateFileW(report_path_.c_str(), FILE_APPEND_DATA | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                    append ? OPEN_ALWAYS : CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);

    bool success{};
    DWORD io_error = ERROR_SUCCESS;
    if (file != INVALID_HANDLE_VALUE) {
        if (append) {
            LARGE_INTEGER end{};
            if (SetFilePointerEx(file, end, nullptr, FILE_END) == 0) {
                io_error = GetLastError();
            }
        }

        DWORD written{};
        // Only a complete write and flush establishes file_started_; initial failures retry with CREATE_ALWAYS.
        if (io_error == ERROR_SUCCESS && WriteFile(file, workspace_->report.data(), static_cast<DWORD>(size), &written, nullptr) != 0 &&
            written == size && FlushFileBuffers(file) != 0) {
            success = true;
        } else if (io_error == ERROR_SUCCESS) {
            io_error = GetLastError();
            if (io_error == ERROR_SUCCESS) {
                io_error = ERROR_WRITE_FAULT;
            }
        }
        CloseHandle(file);
    } else {
        io_error = GetLastError();
    }

    if (success) {
        file_started_ = true;
    } else {
        // Use fixed storage for debugger and console diagnostics when the crash file cannot be written.
        std::array<char, 256> fallback{};
        ReportWriter debug{fallback};
        debug.append("Could not write the Fusion Cutter crash report; exception or fatal error code=");
        debug.hex(code);
        debug.append(", address=");
        debug.hex(address);
        debug.append(", Windows error=");
        debug.decimal(io_error);
        debug.append("\r\n");
        write_fallback({fallback.data(), debug.size()});
    }
}

} // namespace fc::runtime
