#include "crash_reporter.hpp"

#include "crash_report.hpp"

namespace fc::runtime {

using namespace crash_detail;

namespace {

// Describes one expected access violation and links it to an enclosing expectation on this thread.
struct ExpectedFaultState {
    std::uintptr_t instruction{};
    std::uint32_t kind{};
    const ExpectedFaultState *previous{};
};

inline thread_local const ExpectedFaultState *expected_fault_state{};

// Selects native fault codes for capture, excluding reporter inspection and this thread's expected access violations.
bool qualifying_exception(const EXCEPTION_RECORD &record) noexcept {
    // These suppressed faults must not consume report attempts or reenter the shared workspace.
    if (reporter_read_active || CrashReporter::expected_fault(record)) {
        return false;
    }

    switch (record.ExceptionCode) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_IN_PAGE_ERROR:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
    case EXCEPTION_DATATYPE_MISALIGNMENT:
    case EXCEPTION_FLT_DENORMAL_OPERAND:
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:
    case EXCEPTION_FLT_INEXACT_RESULT:
    case EXCEPTION_FLT_INVALID_OPERATION:
    case EXCEPTION_FLT_OVERFLOW:
    case EXCEPTION_FLT_STACK_CHECK:
    case EXCEPTION_FLT_UNDERFLOW:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_INT_OVERFLOW:
    case EXCEPTION_STACK_OVERFLOW:
        return true;
    default:
        return false;
    }
}

// Returns the report label for a known Windows exception code, or UNKNOWN for an unrecognized code.
std::string_view exception_name(std::uint32_t code) noexcept {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
        return "ACCESS_VIOLATION";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
        return "ARRAY_BOUNDS_EXCEEDED";
    case EXCEPTION_DATATYPE_MISALIGNMENT:
        return "DATATYPE_MISALIGNMENT";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:
        return "FLOAT_DIVIDE_BY_ZERO";
    case EXCEPTION_FLT_DENORMAL_OPERAND:
        return "FLOAT_DENORMAL_OPERAND";
    case EXCEPTION_FLT_INEXACT_RESULT:
        return "FLOAT_INEXACT_RESULT";
    case EXCEPTION_FLT_INVALID_OPERATION:
        return "FLOAT_INVALID_OPERATION";
    case EXCEPTION_FLT_OVERFLOW:
        return "FLOAT_OVERFLOW";
    case EXCEPTION_FLT_STACK_CHECK:
        return "FLOAT_STACK_CHECK";
    case EXCEPTION_FLT_UNDERFLOW:
        return "FLOAT_UNDERFLOW";
    case EXCEPTION_ILLEGAL_INSTRUCTION:
        return "ILLEGAL_INSTRUCTION";
    case EXCEPTION_IN_PAGE_ERROR:
        return "IN_PAGE_ERROR";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
        return "INTEGER_DIVIDE_BY_ZERO";
    case EXCEPTION_INT_OVERFLOW:
        return "INTEGER_OVERFLOW";
    case EXCEPTION_PRIV_INSTRUCTION:
        return "PRIVILEGED_INSTRUCTION";
    case EXCEPTION_STACK_OVERFLOW:
        return "STACK_OVERFLOW";
    case 0xc0000374U:
        return "HEAP_CORRUPTION";
    case 0xc0000409U:
        return "STACK_BUFFER_OVERRUN";
    case kStatusFatalAppExit:
        return "FATAL_APP_EXIT";
    default:
        return "UNKNOWN";
    }
}

// Appends one labeled register without changing the architecture's explicit display order.
template <class T> void append_register(ReportWriter &report, std::string_view label, T value) noexcept {
    report.append(label);
    report.hex(value);
}

} // namespace

long __stdcall CrashReporter::vectored_handler(_EXCEPTION_POINTERS *exception) noexcept {
    auto *reporter = active_.load(std::memory_order_acquire);
    return reporter == nullptr ? EXCEPTION_CONTINUE_SEARCH : reporter->capture(exception);
}

long CrashReporter::capture(_EXCEPTION_POINTERS *exception) noexcept {
    if (exception == nullptr || exception->ExceptionRecord == nullptr || exception->ContextRecord == nullptr) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    const auto code = exception->ExceptionRecord->ExceptionCode;
    if (!qualifying_exception(*exception->ExceptionRecord)) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    // Claim the workspace without waiting; recursive or overlapping captures must let exception handling continue.
    std::uint32_t attempt{};
    if (!begin_report(attempt)) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    ReportWriter report{workspace_->report};
    const auto *captured = annotations_.load(std::memory_order_acquire);
    report.append("Fusion Cutter first-chance exception report\r\nReport: ");
    report.decimal(attempt + 1);
    report.append(" of ");
    report.decimal(kMaximumCrashAttempts);
    report.append("\r\n");
    append_session(report);

    // Record the exception type and faulting access before register and memory details can fill the buffer.
    const auto *record = exception->ExceptionRecord;
    report.append("\r\nException code: ");
    report.hex(code);
    report.append(" (");
    report.append(exception_name(code));
    report.append(")");
    report.append("\r\nFlags: ");
    report.hex(record->ExceptionFlags);
    report.append("\r\nInstruction address: ");
    append_address(report, captured, reinterpret_cast<std::uintptr_t>(record->ExceptionAddress));
    report.append("\r\nParameters:");
    for (DWORD index = 0; index < std::min<DWORD>(record->NumberParameters, EXCEPTION_MAXIMUM_PARAMETERS); ++index) {
        report.append(" ");
        report.hex(record->ExceptionInformation[index]);
    }
    report.append("\r\n");
    if ((code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR) && record->NumberParameters >= 2) {
        report.append("Access violation: ");
        report.append(record->ExceptionInformation[0] == 0 ? "read" : record->ExceptionInformation[0] == 1 ? "write" : "execute");
        report.append(" at ");
        append_address(report, captured, record->ExceptionInformation[1]);
        report.append("\r\n");
    }

    // Record the captured registers, including argument registers useful for diagnosing native calls and hooks.
    const auto *context = exception->ContextRecord;
#if defined(_M_X64)
    const auto instruction_pointer = static_cast<std::uintptr_t>(context->Rip);
    const auto stack_pointer = static_cast<std::uintptr_t>(context->Rsp);
    append_register(report, "RIP: ", context->Rip);
    append_register(report, " RSP: ", context->Rsp);
    append_register(report, " RBP: ", context->Rbp);
    append_register(report, " RFLAGS: ", context->EFlags);
    append_register(report, "\r\nRAX: ", context->Rax);
    append_register(report, " RBX: ", context->Rbx);
    append_register(report, " RCX: ", context->Rcx);
    append_register(report, " RDX: ", context->Rdx);
    append_register(report, "\r\nRSI: ", context->Rsi);
    append_register(report, " RDI: ", context->Rdi);
    append_register(report, " R8: ", context->R8);
    append_register(report, " R9: ", context->R9);
    append_register(report, "\r\nR10: ", context->R10);
    append_register(report, " R11: ", context->R11);
    append_register(report, " R12: ", context->R12);
    append_register(report, " R13: ", context->R13);
    append_register(report, "\r\nR14: ", context->R14);
    append_register(report, " R15: ", context->R15);
    report.append("\r\nXMM0: ");
    report.hex(static_cast<std::uint64_t>(context->Xmm0.High));
    report.hex(context->Xmm0.Low);
    report.append(" XMM1: ");
    report.hex(static_cast<std::uint64_t>(context->Xmm1.High));
    report.hex(context->Xmm1.Low);
    report.append("\r\nXMM2: ");
    report.hex(static_cast<std::uint64_t>(context->Xmm2.High));
    report.hex(context->Xmm2.Low);
    report.append(" XMM3: ");
    report.hex(static_cast<std::uint64_t>(context->Xmm3.High));
    report.hex(context->Xmm3.Low);
    report.append("\r\n");
#else
    const auto instruction_pointer = static_cast<std::uintptr_t>(context->Eip);
    const auto stack_pointer = static_cast<std::uintptr_t>(context->Esp);
    append_register(report, "EIP: ", context->Eip);
    append_register(report, " ESP: ", context->Esp);
    append_register(report, " EBP: ", context->Ebp);
    append_register(report, " EFLAGS: ", context->EFlags);
    append_register(report, "\r\nEAX: ", context->Eax);
    append_register(report, " EBX: ", context->Ebx);
    append_register(report, " ECX: ", context->Ecx);
    append_register(report, " EDX: ", context->Edx);
    append_register(report, " ESI: ", context->Esi);
    append_register(report, " EDI: ", context->Edi);
    report.append("\r\n");
#endif

    // Inspect the stack and nearby memory through guarded reads that can tolerate damaged process state.
    append_stack_frames(report, *workspace_, captured, *context);
    append_memory_windows(report, *workspace_, captured, instruction_pointer, stack_pointer);

    report.append(
        "\r\nThis report records an exception before normal exception handling. It does not establish that the process terminated "
        "or that any module, plugin or patch named here caused the exception.\r\n");
    if (attempt + 1 == kMaximumCrashAttempts) {
        report.append("This process has reached the limit on crash report attempts.\r\n");
    }

    append_inventory(report, captured);
    write_report(report.size(), code, reinterpret_cast<std::uintptr_t>(record->ExceptionAddress));

    // Release the workspace and let Windows continue searching for the application's exception handler.
    handling_.clear(std::memory_order_release);
    return EXCEPTION_CONTINUE_SEARCH;
}

void CrashReporter::capture_fatal(std::string_view reason, std::uint32_t code, const PatchRecord *owner) noexcept {
    // Fatal reports use the same exclusive workspace and attempt limit as first-chance captures.
    std::uint32_t attempt{};
    if (!begin_report(attempt)) {
        std::array<char, 1024> fallback{};
        // Preserve the fatal reason in debugger/console output when the report workspace is unavailable.
        ReportWriter message{std::span{fallback}.first(fallback.size() - 1)};
        message.append("Fusion Cutter deliberate fatal termination; crash reporting is unavailable or its attempt limit has been reached. "
                       "Reason: ");
        message.append_structured(reason);
        if (code) {
            message.append("; Windows error: ");
            message.decimal(code);
        }
        message.append("\r\n");
        write_fallback({fallback.data(), message.size()});
        return;
    }

    ReportWriter report{workspace_->report};
    report.append("Fusion Cutter deliberate fatal termination\r\nReport: ");
    report.decimal(attempt + 1);
    report.append(" of ");
    report.decimal(kMaximumCrashAttempts);
    report.append("\r\nReason: ");
    report.append_structured(reason);
    if (code) {
        report.append("\r\nWindows error: ");
        report.decimal(code);
    }
    report.append("\r\n");

    const auto *facts = annotations_.load(std::memory_order_acquire);
    append_session(report);
    if (owner) {
        if (const auto *identity = find_owner(facts, owner)) {
            report.append("Patch: ");
            report.append_structured(identity->label);
            report.append("\r\n");
        }
    }

    if (attempt + 1 == kMaximumCrashAttempts) {
        report.append("This process has reached the limit on crash report attempts.\r\n");
    }

    append_inventory(report, facts);
    write_report(report.size(), code, 0);
    // A failed file write still leaves this invocation responsible for releasing the workspace.
    handling_.clear(std::memory_order_release);
}

namespace {

// Matches an access violation to one scope's exact instruction and read/write/execute kind.
bool matches_expected_fault(const EXCEPTION_RECORD &record, const ExpectedFaultState &expected) noexcept {
    if (record.ExceptionCode != EXCEPTION_ACCESS_VIOLATION || record.NumberParameters < 2 ||
        reinterpret_cast<std::uintptr_t>(record.ExceptionAddress) != expected.instruction) {
        return false;
    }

    const auto discriminator = expected.kind == FC_FAULT_READ ? 0U : expected.kind == FC_FAULT_WRITE ? 1U : 8U;
    return record.ExceptionInformation[0] == discriminator;
}

} // namespace

bool CrashReporter::expected_fault(const EXCEPTION_RECORD &record) noexcept {
    // An outer match suppresses reporting even before SEH unwinds an unmatched inner invocation.
    for (auto scope = expected_fault_state; scope; scope = scope->previous) {
        if (matches_expected_fault(record, *scope)) {
            return true;
        }
    }

    return false;
}

std::uint32_t CrashReporter::invoke_expected_fault(std::uintptr_t instruction, std::uint32_t kind, FC_ExpectedFaultCallback callback,
                                                   void *context) noexcept {
    if (!instruction || kind < FC_FAULT_READ || kind > FC_FAULT_EXECUTE || !callback) {
        return 0;
    }

    const ExpectedFaultState scope{instruction, kind, expected_fault_state};
    expected_fault_state = &scope;
    std::uint32_t outcome = 1;

    // SEH searches before unwinding inner scopes; this filter must match its own invocation.
    __try {
        __try {
            callback(context);
        } __except (matches_expected_fault(*GetExceptionInformation()->ExceptionRecord, scope) ? EXCEPTION_EXECUTE_HANDLER
                                                                                               : EXCEPTION_CONTINUE_SEARCH) {
            outcome = 2;
        }
    } __finally {
        // Restore the enclosing expectation on both normal return and exception unwinding.
        expected_fault_state = scope.previous;
    }

    return outcome;
}

} // namespace fc::runtime
