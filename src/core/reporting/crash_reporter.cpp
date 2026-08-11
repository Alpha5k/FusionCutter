#include "crash_reporter.hpp"

#include "bounded_writer.hpp"

#include <Windows.h>
#include <Psapi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <string_view>

namespace fusioncutter::reporting {
namespace {

constexpr std::size_t kReportCapacity = 64 * 1024;
constexpr std::uint32_t kMaximumReports = 4;
constexpr std::size_t kMaximumModules = 32;
constexpr std::size_t kMaximumInstalledPatches = 128;
constexpr std::size_t kMaximumExecutableRegions = 256;
constexpr std::size_t kMaximumStackFrames = 32;
constexpr std::size_t kMaximumStackCandidates = 40;
constexpr std::size_t kStackWindowSize = 256;
constexpr std::size_t kStackScanSize = 0x800;
constexpr std::size_t kInstructionWindowSize = 32;
constexpr std::uint32_t kStatusFatalAppExit = 0x40000015;
constexpr std::string_view kTruncationMarker = "\r\n[report truncated]\r\n";
constexpr std::wstring_view kCrashFilename = L"FusionCutter-Crash.log";

struct PublishedRegion {
    std::uintptr_t address;
    std::size_t size;
    const char* owner;
    ExecutableRegionKind kind;
};

struct ModuleRecord {
    std::uintptr_t base;
    std::size_t size;
    std::uint64_t preferred_base;
    std::uint32_t timestamp;
    std::array<char, MAX_PATH> basename;
};

struct CrashWorkspace {
    std::array<char, kReportCapacity> report;
    std::array<ModuleRecord, kMaximumModules> modules;
    std::size_t module_count;
    CONTEXT unwind_context;
    std::array<std::byte, kStackScanSize> stack_scan;
    std::array<std::byte, kInstructionWindowSize> instruction_window;
};

struct CrashState {
    std::atomic<void*> handler{nullptr};
    std::atomic_flag reporting = ATOMIC_FLAG_INIT;
    std::atomic<std::uint32_t> report_count{0};

    HostRole role{HostRole::Client};
    std::array<wchar_t, 32'768> report_path{};
    std::array<char, MAX_PATH> executable_basename{};
    std::array<char, 64> selected_proxy{};
    FILETIME process_creation_time{};
    std::uint32_t windows_major{};
    std::uint32_t windows_minor{};
    std::uint32_t windows_build{};

    std::atomic<int> target_layout{-1};
    std::atomic<int> target_architecture{-1};
    std::atomic<const char*> target_fingerprint{nullptr};
    std::atomic<std::size_t> target_fingerprint_length{0};
    std::atomic<CorePhase> phase{CorePhase::Startup};
    std::atomic<const char*> current_patch{nullptr};

    std::array<const char*, kMaximumInstalledPatches> installed_patches{};
    std::atomic<std::size_t> installed_patch_count{0};
    std::array<PublishedRegion, kMaximumExecutableRegions> executable_regions{};
    std::atomic<std::size_t> executable_region_count{0};
};

CrashState g_state;
CrashWorkspace g_workspace;
INIT_ONCE g_install_once = INIT_ONCE_STATIC_INIT;
CrashReporterError g_install_error{"crash reporter installation did not run", ERROR_GEN_FAILURE};
thread_local std::uint32_t g_expected_fault_depth = 0;

using ReportWriter = detail::BoundedWriter;

[[nodiscard]] constexpr bool add_overflows(std::uintptr_t value, std::size_t increment) noexcept {
    return increment > std::numeric_limits<std::uintptr_t>::max() - value;
}

[[nodiscard]] bool protection_is_readable(DWORD protection) noexcept {
    if ((protection & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
        return false;
    }

    switch (protection & 0xFF) {
    case PAGE_READONLY:
    case PAGE_READWRITE:
    case PAGE_WRITECOPY:
    case PAGE_EXECUTE_READ:
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] std::size_t readable_extent(std::uintptr_t address, std::size_t maximum) noexcept {
    std::size_t total = 0;
    while (total < maximum && !add_overflows(address, total)) {
        const auto cursor = address + total;
        MEMORY_BASIC_INFORMATION memory{};
        if (VirtualQuery(reinterpret_cast<const void*>(cursor), &memory, sizeof(memory)) == 0 ||
            memory.State != MEM_COMMIT || !protection_is_readable(memory.Protect)) {
            break;
        }

        const auto region_base = reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
        if (cursor < region_base || add_overflows(region_base, memory.RegionSize)) {
            break;
        }
        const auto region_end = region_base + memory.RegionSize;
        if (cursor >= region_end) {
            break;
        }

        const auto chunk = std::min(maximum - total, static_cast<std::size_t>(region_end - cursor));
        total += chunk;
    }
    return total;
}

[[nodiscard]] bool safe_copy(void* destination, const void* source, std::size_t size) noexcept {
    if (size == 0) {
        return true;
    }
    const auto address = reinterpret_cast<std::uintptr_t>(source);
    if (readable_extent(address, size) != size) {
        return false;
    }

    __try {
        std::memcpy(destination, source, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void copy_bounded_string(std::span<char> destination, std::string_view source) noexcept {
    if (destination.empty()) {
        return;
    }
    const auto length = std::min(destination.size() - 1, source.size());
    if (length != 0) {
        std::memcpy(destination.data(), source.data(), length);
    }
    destination[length] = '\0';
}

[[nodiscard]] const char* basename_from_path(const char* path) noexcept {
    const std::string_view full_path{path};
    const auto separator = full_path.find_last_of("\\/");
    return path + (separator == std::string_view::npos ? 0 : separator + 1);
}

[[nodiscard]] bool read_module_pe(HMODULE module, std::size_t image_size, ModuleRecord& output) noexcept {
    IMAGE_DOS_HEADER dos{};
    if (!safe_copy(&dos, module, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0) {
        return false;
    }

    const auto nt_offset = static_cast<std::size_t>(dos.e_lfanew);
    constexpr auto kNtPrefixSize = sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER);
    if (nt_offset > image_size || kNtPrefixSize > image_size - nt_offset) {
        return false;
    }

    const auto nt_address = reinterpret_cast<std::uintptr_t>(module) + nt_offset;
    DWORD signature = 0;
    IMAGE_FILE_HEADER file_header{};
    if (!safe_copy(&signature, reinterpret_cast<const void*>(nt_address), sizeof(signature)) ||
        signature != IMAGE_NT_SIGNATURE ||
        !safe_copy(&file_header, reinterpret_cast<const void*>(nt_address + sizeof(signature)), sizeof(file_header))) {
        return false;
    }

    const auto optional_offset = nt_offset + kNtPrefixSize;
    if (file_header.SizeOfOptionalHeader > image_size - optional_offset) {
        return false;
    }
    const auto optional_address = nt_address + kNtPrefixSize;
    WORD optional_magic = 0;
    if (!safe_copy(&optional_magic, reinterpret_cast<const void*>(optional_address), sizeof(optional_magic))) {
        return false;
    }

    if (optional_magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
        IMAGE_OPTIONAL_HEADER32 optional{};
        if (file_header.SizeOfOptionalHeader < sizeof(optional) ||
            !safe_copy(&optional, reinterpret_cast<const void*>(optional_address), sizeof(optional))) {
            return false;
        }
        output.preferred_base = optional.ImageBase;
    } else if (optional_magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        IMAGE_OPTIONAL_HEADER64 optional{};
        if (file_header.SizeOfOptionalHeader < sizeof(optional) ||
            !safe_copy(&optional, reinterpret_cast<const void*>(optional_address), sizeof(optional))) {
            return false;
        }
        output.preferred_base = optional.ImageBase;
    } else {
        return false;
    }

    output.timestamp = file_header.TimeDateStamp;
    return true;
}

[[nodiscard]] bool resolve_module(std::uintptr_t address, ModuleRecord& output) noexcept {
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(address), &module) ||
        module == nullptr) {
        return false;
    }

    MODULEINFO information{};
    if (!K32GetModuleInformation(GetCurrentProcess(), module, &information, sizeof(information)) ||
        information.lpBaseOfDll == nullptr || information.SizeOfImage == 0) {
        return false;
    }

    output = {};
    output.base = reinterpret_cast<std::uintptr_t>(information.lpBaseOfDll);
    output.size = information.SizeOfImage;
    if (address < output.base || address - output.base >= output.size) {
        return false;
    }

    const auto basename_length = K32GetModuleBaseNameA(GetCurrentProcess(), module, output.basename.data(),
                                                       static_cast<DWORD>(output.basename.size()));
    if (basename_length == 0 || basename_length >= output.basename.size()) {
        copy_bounded_string(output.basename, "UnknownModule");
    }
    static_cast<void>(read_module_pe(module, output.size, output));
    return true;
}

[[nodiscard]] ModuleRecord* find_module(std::uintptr_t address) noexcept {
    for (std::size_t index = 0; index < g_workspace.module_count; ++index) {
        auto& module = g_workspace.modules[index];
        if (address >= module.base && address - module.base < module.size) {
            return &module;
        }
    }

    if (g_workspace.module_count == g_workspace.modules.size()) {
        return nullptr;
    }
    auto& candidate = g_workspace.modules[g_workspace.module_count];
    if (!resolve_module(address, candidate)) {
        return nullptr;
    }
    ++g_workspace.module_count;
    return &candidate;
}

[[nodiscard]] const PublishedRegion* find_executable_region(std::uintptr_t address) noexcept {
    const auto count = g_state.executable_region_count.load(std::memory_order_acquire);
    for (std::size_t index = 0; index < count; ++index) {
        const auto& region = g_state.executable_regions[index];
        if (address >= region.address && address - region.address < region.size) {
            return &region;
        }
    }
    return nullptr;
}

[[nodiscard]] constexpr std::string_view region_kind_name(ExecutableRegionKind kind) noexcept {
    switch (kind) {
    case ExecutableRegionKind::PatchSite:
        return "PatchSite";
    case ExecutableRegionKind::Hook:
        return "Hook";
    case ExecutableRegionKind::Trampoline:
        return "Trampoline";
    case ExecutableRegionKind::Relay:
        return "Relay";
    case ExecutableRegionKind::CoreAllocation:
        return "CoreAllocation";
    }
    return "Unknown";
}

void append_address(ReportWriter& output, std::uintptr_t address) noexcept {
    output.append_pointer(address);
    if (auto* module = find_module(address); module != nullptr) {
        const auto rva = address - module->base;
        output.append(" (");
        output.append_c_string(module->basename.data(), module->basename.size());
        output.append("+0x");
        output.append_hex(rva);
        if (module->preferred_base != 0 && rva <= std::numeric_limits<std::uint64_t>::max() - module->preferred_base) {
            output.append(", preferred=0x");
            output.append_hex(module->preferred_base + rva);
        }
        output.append(")");
    }

    if (const auto* region = find_executable_region(address); region != nullptr) {
        output.append(" [");
        output.append(region_kind_name(region->kind));
        output.append(" owner=");
        output.append_c_string(region->owner, 128);
        output.append("]");
    }
}

[[nodiscard]] constexpr std::string_view layout_name(int layout) noexcept {
    switch (static_cast<TargetLayout>(layout)) {
    case TargetLayout::SteamRetail:
        return "SteamRetail";
    case TargetLayout::GOGRetail:
        return "GOGRetail";
    case TargetLayout::Aspyr:
        return "Aspyr";
    case TargetLayout::ModTools:
        return "ModTools";
    }
    return "Unknown";
}

[[nodiscard]] constexpr std::string_view role_name(HostRole role) noexcept {
    return role == HostRole::Client ? "Client" : "Server";
}

[[nodiscard]] constexpr std::string_view architecture_name(int architecture) noexcept {
    switch (static_cast<Architecture>(architecture)) {
    case Architecture::X86:
        return "x86";
    case Architecture::X64:
        return "x64";
    }
#if defined(_M_IX86)
    return "x86 (target unknown)";
#else
    return "x64 (target unknown)";
#endif
}

[[nodiscard]] constexpr std::string_view phase_name(CorePhase phase) noexcept {
    switch (phase) {
    case CorePhase::Startup:
        return "Startup";
    case CorePhase::TargetRecognition:
        return "TargetRecognition";
    case CorePhase::Configuration:
        return "Configuration";
    case CorePhase::PatchSelection:
        return "PatchSelection";
    case CorePhase::PatchPlanning:
        return "PatchPlanning";
    case CorePhase::PatchValidation:
        return "PatchValidation";
    case CorePhase::PatchCommit:
        return "PatchCommit";
    case CorePhase::PatchActivation:
        return "PatchActivation";
    case CorePhase::Runtime:
        return "Runtime";
    case CorePhase::Completed:
        return "Completed";
    case CorePhase::Failed:
        return "Failed";
    }
    return "Unknown";
}

[[nodiscard]] constexpr std::string_view exception_name(std::uint32_t code) noexcept {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
        return "ACCESS_VIOLATION";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
        return "ARRAY_BOUNDS_EXCEEDED";
    case EXCEPTION_DATATYPE_MISALIGNMENT:
        return "DATATYPE_MISALIGNMENT";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:
        return "FLOAT_DIVIDE_BY_ZERO";
    case EXCEPTION_ILLEGAL_INSTRUCTION:
        return "ILLEGAL_INSTRUCTION";
    case EXCEPTION_IN_PAGE_ERROR:
        return "IN_PAGE_ERROR";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
        return "INTEGER_DIVIDE_BY_ZERO";
    case EXCEPTION_PRIV_INSTRUCTION:
        return "PRIVILEGED_INSTRUCTION";
    case EXCEPTION_STACK_OVERFLOW:
        return "STACK_OVERFLOW";
    case 0xC0000374:
        return "HEAP_CORRUPTION";
    case 0xC0000409:
        return "STACK_BUFFER_OVERRUN";
    case kStatusFatalAppExit:
        return "FATAL_APP_EXIT";
    default:
        return "UNKNOWN";
    }
}

[[nodiscard]] bool qualifies_for_report(std::uint32_t code) noexcept {
    if (g_expected_fault_depth != 0) {
        return false;
    }
    return code == kStatusFatalAppExit || ((code & 0xC0000000U) == 0xC0000000U && (code & 0x20000000U) == 0);
}

[[nodiscard]] bool reserve_report(std::uint32_t& report_number) noexcept {
    auto current = g_state.report_count.load(std::memory_order_relaxed);
    while (current < kMaximumReports) {
        if (g_state.report_count.compare_exchange_weak(current, current + 1, std::memory_order_relaxed)) {
            report_number = current + 1;
            return true;
        }
    }
    return false;
}

void append_utc_time(ReportWriter& output, const FILETIME& time) noexcept {
    SYSTEMTIME utc{};
    if (!FileTimeToSystemTime(&time, &utc)) {
        output.append("Unknown");
        return;
    }

    output.append_decimal(utc.wYear, 4);
    output.append("-");
    output.append_decimal(utc.wMonth, 2);
    output.append("-");
    output.append_decimal(utc.wDay, 2);
    output.append("T");
    output.append_decimal(utc.wHour, 2);
    output.append(":");
    output.append_decimal(utc.wMinute, 2);
    output.append(":");
    output.append_decimal(utc.wSecond, 2);
    output.append(".");
    output.append_decimal(utc.wMilliseconds, 3);
    output.append("Z");
}

[[nodiscard]] std::uint64_t filetime_value(const FILETIME& time) noexcept {
    ULARGE_INTEGER value{};
    value.LowPart = time.dwLowDateTime;
    value.HighPart = time.dwHighDateTime;
    return value.QuadPart;
}

void append_process_identity(ReportWriter& output, std::uint32_t report_number) noexcept {
    FILETIME now{};
    GetSystemTimeAsFileTime(&now);
    const auto now_value = filetime_value(now);
    const auto creation_value = filetime_value(g_state.process_creation_time);
    const auto uptime_ms = now_value >= creation_value ? (now_value - creation_value) / 10'000 : 0;
    const auto* fingerprint = g_state.target_fingerprint.load(std::memory_order_acquire);
    const auto fingerprint_length = g_state.target_fingerprint_length.load(std::memory_order_relaxed);
    const auto target_layout = g_state.target_layout.load(std::memory_order_relaxed);
    const auto target_architecture = g_state.target_architecture.load(std::memory_order_relaxed);

    output.append("Fusion Cutter first-chance exception report\r\n");
    output.append("This report records an observed exception and does not claim that the process terminated.\r\n\r\n");
    output.append("ReportNumber: ");
    output.append_decimal(report_number);
    output.append("\r\nUtcTime: ");
    append_utc_time(output, now);
    output.append("\r\nProcessUptimeMs: ");
    output.append_decimal(uptime_ms);
    output.append("\r\nProcessId: ");
    output.append_decimal(GetCurrentProcessId());
    output.append("\r\nThreadId: ");
    output.append_decimal(GetCurrentThreadId());
    output.append("\r\nFusionCutterVersion: " FC_VERSION_STRING);
    output.append("\r\nBuildId: " FC_BUILD_ID);
    output.append("\r\nWindows: ");
    output.append_decimal(g_state.windows_major);
    output.append(".");
    output.append_decimal(g_state.windows_minor);
    output.append(" build ");
    output.append_decimal(g_state.windows_build);
    output.append("\r\nExecutable: ");
    output.append_c_string(g_state.executable_basename.data(), g_state.executable_basename.size());
    output.append("\r\nRole: ");
    output.append(role_name(g_state.role));
    output.append("\r\nArchitecture: ");
    output.append(architecture_name(target_architecture));
    output.append("\r\nTarget: ");
    output.append(layout_name(target_layout));
    output.append("\r\nTargetFingerprint: ");
    output.append(fingerprint == nullptr ? std::string_view{"None"}
                                         : std::string_view{fingerprint, fingerprint_length});
    output.append("\r\nSelectedDirectInputProxy: ");
    if (g_state.selected_proxy.front() == '\0') {
        output.append("None");
    } else {
        output.append_c_string(g_state.selected_proxy.data(), g_state.selected_proxy.size());
    }
    output.append("\r\nCorePhase: ");
    output.append(phase_name(g_state.phase.load(std::memory_order_acquire)));
    output.append("\r\nCurrentPatch: ");
    output.append_c_string(g_state.current_patch.load(std::memory_order_acquire), 128);
    output.append("\r\nInstalledPatches:");

    const auto installed_count = g_state.installed_patch_count.load(std::memory_order_acquire);
    if (installed_count == 0) {
        output.append(" None\r\n");
        return;
    }
    output.append("\r\n");
    for (std::size_t index = 0; index < installed_count; ++index) {
        output.append("  - ");
        output.append_c_string(g_state.installed_patches[index], 128);
        output.append("\r\n");
    }
}

void append_exception(ReportWriter& output, const EXCEPTION_RECORD& exception) noexcept {
    output.append("\r\nException\r\nCode: 0x");
    output.append_hex(exception.ExceptionCode, 8);
    output.append(" (");
    output.append(exception_name(exception.ExceptionCode));
    output.append(")\r\nFlags: 0x");
    output.append_hex(exception.ExceptionFlags, 8);
    output.append("\r\nInstruction: ");
    append_address(output, reinterpret_cast<std::uintptr_t>(exception.ExceptionAddress));
    output.append("\r\nParameters:");
    if (exception.NumberParameters == 0) {
        output.append(" None\r\n");
    } else {
        output.append("\r\n");
        const auto count = std::min<std::uint32_t>(exception.NumberParameters, EXCEPTION_MAXIMUM_PARAMETERS);
        for (std::uint32_t index = 0; index < count; ++index) {
            output.append("  [");
            output.append_decimal(index);
            output.append("]=0x");
            output.append_hex(exception.ExceptionInformation[index], sizeof(ULONG_PTR) * 2);
            output.append("\r\n");
        }
    }

    if ((exception.ExceptionCode == EXCEPTION_ACCESS_VIOLATION || exception.ExceptionCode == EXCEPTION_IN_PAGE_ERROR) &&
        exception.NumberParameters >= 2) {
        output.append("Access: ");
        switch (exception.ExceptionInformation[0]) {
        case 0:
            output.append("Read");
            break;
        case 1:
            output.append("Write");
            break;
        case 8:
            output.append("Execute");
            break;
        default:
            output.append("Unknown");
            break;
        }
        output.append(" at ");
        append_address(output, exception.ExceptionInformation[1]);
        output.append("\r\n");
    }
}

void append_register_line(ReportWriter& output, std::string_view name, std::uintptr_t value) noexcept {
    output.append(name);
    output.append("=");
    output.append_pointer(value);
}

void append_registers(ReportWriter& output, const CONTEXT& context) noexcept {
    output.append("\r\nRegisters\r\n");
#if defined(_M_X64)
    append_register_line(output, "RAX", context.Rax);
    output.append("  ");
    append_register_line(output, "RBX", context.Rbx);
    output.append("  ");
    append_register_line(output, "RCX", context.Rcx);
    output.append("  ");
    append_register_line(output, "RDX", context.Rdx);
    output.append("\r\n");
    append_register_line(output, "RSI", context.Rsi);
    output.append("  ");
    append_register_line(output, "RDI", context.Rdi);
    output.append("  ");
    append_register_line(output, "RBP", context.Rbp);
    output.append("  ");
    append_register_line(output, "RSP", context.Rsp);
    output.append("\r\n");
    append_register_line(output, "R8 ", context.R8);
    output.append("  ");
    append_register_line(output, "R9 ", context.R9);
    output.append("  ");
    append_register_line(output, "R10", context.R10);
    output.append("  ");
    append_register_line(output, "R11", context.R11);
    output.append("\r\n");
    append_register_line(output, "R12", context.R12);
    output.append("  ");
    append_register_line(output, "R13", context.R13);
    output.append("  ");
    append_register_line(output, "R14", context.R14);
    output.append("  ");
    append_register_line(output, "R15", context.R15);
    output.append("\r\n");
    append_register_line(output, "RIP", context.Rip);
    output.append("  EFlags=0x");
    output.append_hex(context.EFlags, 8);
    output.append("\r\nXMM0=0x");
    output.append_hex(static_cast<std::uint64_t>(context.Xmm0.High), 16);
    output.append_hex(context.Xmm0.Low, 16);
    output.append("  XMM1=0x");
    output.append_hex(static_cast<std::uint64_t>(context.Xmm1.High), 16);
    output.append_hex(context.Xmm1.Low, 16);
    output.append("\r\nXMM2=0x");
    output.append_hex(static_cast<std::uint64_t>(context.Xmm2.High), 16);
    output.append_hex(context.Xmm2.Low, 16);
    output.append("  XMM3=0x");
    output.append_hex(static_cast<std::uint64_t>(context.Xmm3.High), 16);
    output.append_hex(context.Xmm3.Low, 16);
    output.append("\r\n");
#elif defined(_M_IX86)
    append_register_line(output, "EAX", context.Eax);
    output.append("  ");
    append_register_line(output, "EBX", context.Ebx);
    output.append("  ");
    append_register_line(output, "ECX", context.Ecx);
    output.append("  ");
    append_register_line(output, "EDX", context.Edx);
    output.append("\r\n");
    append_register_line(output, "ESI", context.Esi);
    output.append("  ");
    append_register_line(output, "EDI", context.Edi);
    output.append("  ");
    append_register_line(output, "EBP", context.Ebp);
    output.append("  ");
    append_register_line(output, "ESP", context.Esp);
    output.append("\r\n");
    append_register_line(output, "EIP", context.Eip);
    output.append("  EFlags=0x");
    output.append_hex(context.EFlags, 8);
    output.append("\r\n");
#endif
}

#if defined(_M_X64)
[[nodiscard]] bool unwind_once(CONTEXT& context) noexcept {
    __try {
        DWORD64 image_base = 0;
        auto* function = RtlLookupFunctionEntry(context.Rip, &image_base, nullptr);
        if (function == nullptr) {
            std::uint64_t return_address = 0;
            if (!safe_copy(&return_address, reinterpret_cast<const void*>(context.Rsp), sizeof(return_address))) {
                return false;
            }
            context.Rsp += sizeof(return_address);
            context.Rip = return_address;
            return true;
        }

        void* handler_data = nullptr;
        DWORD64 establisher_frame = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, context.Rip, function, &context, &handler_data,
                         &establisher_frame, nullptr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
#endif

void append_stack_frames(ReportWriter& output, const CONTEXT& context) noexcept {
    output.append("\r\nStackFrames\r\n");
#if defined(_M_X64)
    g_workspace.unwind_context = context;
    std::size_t frame_count = 0;
    while (frame_count < kMaximumStackFrames && g_workspace.unwind_context.Rip != 0) {
        output.append("  #");
        output.append_decimal(frame_count);
        output.append(" ");
        append_address(output, g_workspace.unwind_context.Rip);
        output.append("\r\n");
        ++frame_count;

        const auto previous_ip = g_workspace.unwind_context.Rip;
        const auto previous_sp = g_workspace.unwind_context.Rsp;
        if (!unwind_once(g_workspace.unwind_context) ||
            (g_workspace.unwind_context.Rip == previous_ip && g_workspace.unwind_context.Rsp == previous_sp)) {
            break;
        }
    }
    if (frame_count == 0) {
        output.append("  (no unwindable frames)\r\n");
    }
#else
    output.append("  #0 ");
    append_address(output, context.Eip);
    output.append("\r\n");

    struct Frame {
        std::uint32_t next;
        std::uint32_t return_address;
    };

    auto frame_pointer = static_cast<std::uintptr_t>(context.Ebp);
    for (std::size_t frame_index = 1; frame_index < kMaximumStackFrames; ++frame_index) {
        Frame frame{};
        if (!safe_copy(&frame, reinterpret_cast<const void*>(frame_pointer), sizeof(frame)) ||
            frame.next <= frame_pointer || frame.return_address == 0) {
            break;
        }
        output.append("  #");
        output.append_decimal(frame_index);
        output.append(" ");
        append_address(output, frame.return_address);
        output.append("\r\n");
        frame_pointer = frame.next;
    }
#endif
}

[[nodiscard]] std::uintptr_t stack_pointer(const CONTEXT& context) noexcept {
#if defined(_M_X64)
    return context.Rsp;
#else
    return context.Esp;
#endif
}

[[nodiscard]] std::uintptr_t instruction_pointer(const CONTEXT& context) noexcept {
#if defined(_M_X64)
    return context.Rip;
#else
    return context.Eip;
#endif
}

void append_hex_dump(ReportWriter& output, std::span<const std::byte> bytes, std::uintptr_t base) noexcept {
    for (std::size_t offset = 0; offset < bytes.size(); offset += 16) {
        output.append("  ");
        output.append_pointer(base + offset);
        output.append(":");
        const auto line_size = std::min<std::size_t>(16, bytes.size() - offset);
        for (std::size_t index = 0; index < line_size; ++index) {
            output.append(" ");
            output.append_hex(std::to_integer<std::uint8_t>(bytes[offset + index]), 2);
        }
        output.append("\r\n");
    }
}

void append_memory_windows(ReportWriter& output, const CONTEXT& context) noexcept {
    const auto ip = instruction_pointer(context);
    const auto instruction_size = readable_extent(ip, g_workspace.instruction_window.size());
    output.append("\r\nInstructionBytes\r\n");
    if (instruction_size == 0 ||
        !safe_copy(g_workspace.instruction_window.data(), reinterpret_cast<const void*>(ip), instruction_size)) {
        output.append("  (unavailable)\r\n");
    } else {
        append_hex_dump(output, std::span{g_workspace.instruction_window}.first(instruction_size), ip);
    }

    const auto sp = stack_pointer(context);
    const auto scan_size = readable_extent(sp, g_workspace.stack_scan.size());
    if (scan_size != 0) {
        static_cast<void>(safe_copy(g_workspace.stack_scan.data(), reinterpret_cast<const void*>(sp), scan_size));
    }

    output.append("\r\nStackWindow\r\n");
    const auto window_size = std::min(scan_size, kStackWindowSize);
    if (window_size == 0) {
        output.append("  (unavailable)\r\n");
    } else {
        append_hex_dump(output, std::span{g_workspace.stack_scan}.first(window_size), sp);
    }

    output.append("\r\nStackAddressCandidates\r\n");
    std::size_t candidates = 0;
    for (std::size_t offset = 0; offset + sizeof(std::uintptr_t) <= scan_size && candidates < kMaximumStackCandidates;
         offset += sizeof(std::uintptr_t)) {
        std::uintptr_t value = 0;
        std::memcpy(&value, g_workspace.stack_scan.data() + offset, sizeof(value));
        if (find_module(value) == nullptr) {
            continue;
        }
        output.append("  [SP+0x");
        output.append_hex(offset, 3);
        output.append("] ");
        append_address(output, value);
        output.append("\r\n");
        ++candidates;
    }
    if (candidates == 0) {
        output.append("  (none)\r\n");
    }
}

void append_modules(ReportWriter& output) noexcept {
    output.append("\r\nImplicatedModules\r\n");
    if (g_workspace.module_count == 0) {
        output.append("  (none)\r\n");
        return;
    }

    for (std::size_t index = 0; index < g_workspace.module_count; ++index) {
        const auto& module = g_workspace.modules[index];
        output.append("  ");
        output.append_c_string(module.basename.data(), module.basename.size());
        output.append(" base=");
        output.append_pointer(module.base);
        output.append(" size=0x");
        output.append_hex(module.size);
        output.append(" timestamp=0x");
        output.append_hex(module.timestamp, 8);
        output.append("\r\n");
    }
}

[[nodiscard]] std::span<const char> render_report(const EXCEPTION_POINTERS& pointers,
                                                  std::uint32_t report_number) noexcept {
    g_workspace.module_count = 0;
    ReportWriter output(g_workspace.report, kTruncationMarker);
    append_process_identity(output, report_number);
    append_exception(output, *pointers.ExceptionRecord);
    append_registers(output, *pointers.ContextRecord);
    append_stack_frames(output, *pointers.ContextRecord);
    append_memory_windows(output, *pointers.ContextRecord);
    append_modules(output);
    output.append("\r\nEndOfReport\r\n\r\n");
    return output.finish();
}

[[nodiscard]] bool write_report(std::span<const char> report, std::uint32_t report_number,
                                std::uint32_t& windows_error) noexcept {
    const auto disposition = report_number == 1 ? CREATE_ALWAYS : OPEN_ALWAYS;
    const HANDLE file = CreateFileW(g_state.report_path.data(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    nullptr, disposition, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        windows_error = GetLastError();
        return false;
    }

    std::size_t total = 0;
    bool succeeded = true;
    while (total < report.size()) {
        DWORD written = 0;
        const auto remaining = static_cast<DWORD>(report.size() - total);
        if (!WriteFile(file, report.data() + total, remaining, &written, nullptr) || written == 0) {
            windows_error = GetLastError();
            succeeded = false;
            break;
        }
        total += written;
    }
    if (succeeded && !FlushFileBuffers(file)) {
        windows_error = GetLastError();
        succeeded = false;
    }
    CloseHandle(file);
    return succeeded;
}

void report_file_failure(const EXCEPTION_RECORD& exception, std::uint32_t windows_error) noexcept {
    std::array<char, 512> fallback{};
    ReportWriter output(fallback, kTruncationMarker);
    output.append("Fusion Cutter could not write FusionCutter-Crash.log; exception=0x");
    output.append_hex(exception.ExceptionCode, 8);
    output.append(" address=");
    output.append_pointer(reinterpret_cast<std::uintptr_t>(exception.ExceptionAddress));
    output.append(" windows_error=");
    output.append_decimal(windows_error);
    output.append("\r\n");
    const auto message = output.finish();
    std::array<char, 512> terminated{};
    const auto length = std::min(message.size(), terminated.size() - 1);
    std::memcpy(terminated.data(), message.data(), length);
    OutputDebugStringA(terminated.data());
}

LONG CALLBACK crash_handler(EXCEPTION_POINTERS* pointers) noexcept {
    if (pointers == nullptr || pointers->ExceptionRecord == nullptr || pointers->ContextRecord == nullptr ||
        !qualifies_for_report(pointers->ExceptionRecord->ExceptionCode)) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    if (g_state.reporting.test_and_set(std::memory_order_acquire)) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    std::uint32_t report_number = 0;
    if (reserve_report(report_number)) {
        const auto report = render_report(*pointers, report_number);
        std::uint32_t windows_error = ERROR_SUCCESS;
        if (!write_report(report, report_number, windows_error)) {
            report_file_failure(*pointers->ExceptionRecord, windows_error);
        }
    }

    g_state.reporting.clear(std::memory_order_release);
    return EXCEPTION_CONTINUE_SEARCH;
}

void set_install_error(std::string_view detail, std::uint32_t windows_error) noexcept {
    g_install_error = {detail, windows_error};
}

[[nodiscard]] bool prepare_report_path() noexcept {
    HMODULE owner = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&g_state), &owner) ||
        owner == nullptr) {
        set_install_error("crash reporter module is unavailable", GetLastError());
        return false;
    }

    const auto path_length =
        GetModuleFileNameW(owner, g_state.report_path.data(), static_cast<DWORD>(g_state.report_path.size()));
    if (path_length == 0 || path_length >= g_state.report_path.size()) {
        set_install_error("crash report path is unavailable",
                          path_length == 0 ? GetLastError() : ERROR_INSUFFICIENT_BUFFER);
        return false;
    }

    const std::wstring_view module_path{g_state.report_path.data(), path_length};
    const auto separator = module_path.find_last_of(L"\\/");
    if (separator == std::wstring_view::npos) {
        set_install_error("crash report path does not contain a directory", ERROR_INVALID_NAME);
        return false;
    }

    const auto filename_offset = separator + 1;
    if (kCrashFilename.size() >= g_state.report_path.size() - filename_offset) {
        set_install_error("crash report path cannot contain the report filename", ERROR_INSUFFICIENT_BUFFER);
        return false;
    }

    std::memcpy(g_state.report_path.data() + filename_offset, kCrashFilename.data(),
                kCrashFilename.size() * sizeof(wchar_t));
    g_state.report_path[filename_offset + kCrashFilename.size()] = L'\0';
    return true;
}

void read_windows_version() noexcept {
    using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll == nullptr) {
        return;
    }
    const auto get_version = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"));
    if (get_version == nullptr) {
        return;
    }

    RTL_OSVERSIONINFOW version{};
    version.dwOSVersionInfoSize = sizeof(version);
    if (get_version(&version) == 0) {
        g_state.windows_major = version.dwMajorVersion;
        g_state.windows_minor = version.dwMinorVersion;
        g_state.windows_build = version.dwBuildNumber;
    }
}

struct InstallParameters {
    HostRole role;
    std::string_view selected_proxy;
};

BOOL CALLBACK install_once(INIT_ONCE*, void* parameter, void**) noexcept {
    const auto& install = *static_cast<const InstallParameters*>(parameter);
    g_state.role = install.role;
    copy_bounded_string(g_state.selected_proxy, install.selected_proxy);

    if (!prepare_report_path()) {
        return FALSE;
    }

    std::array<char, 32'768> executable_path{};
    const auto executable_length =
        GetModuleFileNameA(nullptr, executable_path.data(), static_cast<DWORD>(executable_path.size()));
    if (executable_length == 0 || executable_length >= executable_path.size()) {
        set_install_error("process executable name is unavailable",
                          executable_length == 0 ? GetLastError() : ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    copy_bounded_string(g_state.executable_basename, basename_from_path(executable_path.data()));

    FILETIME exit_time{};
    FILETIME kernel_time{};
    FILETIME user_time{};
    if (!GetProcessTimes(GetCurrentProcess(), &g_state.process_creation_time, &exit_time, &kernel_time, &user_time)) {
        set_install_error("process creation time is unavailable", GetLastError());
        return FALSE;
    }
    read_windows_version();

    void* handler = AddVectoredExceptionHandler(1, crash_handler);
    if (handler == nullptr) {
        set_install_error("first-priority vectored exception handler registration failed", GetLastError());
        return FALSE;
    }
    g_state.handler.store(handler, std::memory_order_release);
    return TRUE;
}

} // namespace

std::expected<void, CrashReporterError> install_crash_reporter(HostRole role,
                                                               std::string_view selected_proxy) noexcept {
    const InstallParameters parameters{role, selected_proxy};
    if (!InitOnceExecuteOnce(&g_install_once, install_once, const_cast<InstallParameters*>(&parameters), nullptr)) {
        return std::unexpected(g_install_error);
    }
    return {};
}

void uninstall_crash_reporter() noexcept {
    void* handler = g_state.handler.exchange(nullptr, std::memory_order_acq_rel);
    if (handler != nullptr) {
        static_cast<void>(RemoveVectoredExceptionHandler(handler));
    }
}

void publish_crash_target(const TargetContext& target, std::string_view fingerprint) noexcept {
    g_state.target_layout.store(static_cast<int>(target.layout), std::memory_order_relaxed);
    g_state.target_architecture.store(static_cast<int>(target.image.architecture), std::memory_order_relaxed);
    g_state.target_fingerprint_length.store(fingerprint.size(), std::memory_order_relaxed);
    g_state.target_fingerprint.store(fingerprint.data(), std::memory_order_release);
}

void publish_crash_phase(CorePhase phase, const char* current_patch) noexcept {
    g_state.current_patch.store(current_patch, std::memory_order_relaxed);
    g_state.phase.store(phase, std::memory_order_release);
}

void publish_installed_patch(const char* patch_id) noexcept {
    const auto index = g_state.installed_patch_count.load(std::memory_order_relaxed);
    if (patch_id == nullptr || index == g_state.installed_patches.size()) {
        return;
    }
    g_state.installed_patches[index] = patch_id;
    g_state.installed_patch_count.store(index + 1, std::memory_order_release);
}

bool publish_executable_region(std::uintptr_t address, std::size_t size, const char* owner,
                               ExecutableRegionKind kind) noexcept {
    const auto index = g_state.executable_region_count.load(std::memory_order_relaxed);
    if (address == 0 || size == 0 || add_overflows(address, size) || owner == nullptr ||
        index == g_state.executable_regions.size()) {
        return false;
    }
    g_state.executable_regions[index] = PublishedRegion{address, size, owner, kind};
    g_state.executable_region_count.store(index + 1, std::memory_order_release);
    return true;
}

void begin_expected_fault() noexcept {
    if (g_expected_fault_depth != std::numeric_limits<std::uint32_t>::max()) {
        ++g_expected_fault_depth;
    }
}

void end_expected_fault() noexcept {
    if (g_expected_fault_depth != 0) {
        --g_expected_fault_depth;
    }
}

} // namespace fusioncutter::reporting
