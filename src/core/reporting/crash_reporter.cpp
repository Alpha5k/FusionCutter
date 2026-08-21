#include "crash_reporter.hpp"

#include "../patching/hook_registry.hpp"
#include "../targets/recognition.hpp"

#include <Windows.h>
#include <Psapi.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <concepts>
#include <cstring>
#include <limits>
#include <new>
#include <ranges>
#include <type_traits>
#include <utility>
#include <variant>

namespace fc::runtime {

// A cached module record gives exceptional formatting stable address identity without symbol loading or allocation.
struct CrashModuleRecord {
    std::uintptr_t base{};
    std::size_t size{};
    std::uint64_t preferred_base{};
    std::uint32_t timestamp{};
    std::array<char, MAX_PATH> basename{};
};

// One reusable workspace keeps exceptional reporting off the heap and avoids large buffers on a damaged thread stack.
struct CrashWorkspace {
    std::array<char, 64U * 1024U> report{};
    std::array<CrashModuleRecord, 32> modules{};
    std::size_t module_count{};
    CONTEXT unwind_context{};
    std::array<std::byte, 0x800> stack_scan{};
    std::array<std::byte, 32> instruction_window{};
};

namespace {

inline thread_local std::uint32_t expected_exception_code{};

inline constexpr std::uint32_t kMaximumCrashAttempts = 4;
inline constexpr std::uint32_t kMsvcCppException = 0xe06d7363U;
inline constexpr std::uint32_t kStatusFatalAppExit = 0x40000015U;
inline constexpr std::size_t kMaximumStackFrames = 32;
inline constexpr std::size_t kMaximumStackCandidates = 40;
inline constexpr std::size_t kStackWindowSize = 256;

[[nodiscard]] std::size_t utf8_boundary(std::string_view value, std::size_t limit) noexcept {
    auto result = std::min(limit, value.size());
    while (result != 0 && result < value.size() && (static_cast<unsigned char>(value[result]) & 0xc0U) == 0x80U) {
        --result;
    }
    return result;
}

// Crash fields use the same bounded single-line representation even though the exceptional reader cannot allocate.
template <class Destination> void copy_structured(Destination& destination, std::string_view source) noexcept {
    const auto copied = utf8_boundary(source, std::min(source.size(), std::size(destination) - 1));
    for (std::size_t index = 0; index < copied; ++index) {
        const auto byte = static_cast<unsigned char>(source[index]);
        destination[index] = byte <= 0x1fU || byte == 0x7fU ? ' ' : source[index];
    }
    destination[copied] = '\0';
}

// ReportWriter appends into a reusable 64 KiB crash buffer and reserves a visible truncation marker.
class ReportWriter final {
  public:
    explicit ReportWriter(std::span<char> output) noexcept : output_(output) {}

    void append(std::string_view value) noexcept {
        constexpr std::string_view marker = "\r\n[Crash report truncated]\r\n";
        if (truncated_ || value.empty()) {
            return;
        }
        if (value.size() <= output_.size() - size_) {
            std::memcpy(output_.data() + size_, value.data(), value.size());
            size_ += value.size();
            return;
        }
        truncated_ = true;
        const auto marker_size = std::min(marker.size(), output_.size());
        const auto content_limit = output_.size() - marker_size;
        const auto copied = size_ < content_limit ? std::min(value.size(), content_limit - size_) : 0;
        if (copied != 0) {
            std::memcpy(output_.data() + size_, value.data(), copied);
            size_ += copied;
        }
        std::memcpy(output_.data() + content_limit, marker.data() + marker.size() - marker_size, marker_size);
        size_ = output_.size();
    }

    void append_structured(std::string_view value) noexcept {
        constexpr std::string_view marker = "\r\n[Crash report truncated]\r\n";
        if (truncated_ || value.empty()) {
            return;
        }
        const auto fits = value.size() <= output_.size() - size_;
        const auto marker_size = std::min(marker.size(), output_.size());
        const auto content_limit = output_.size() - marker_size;
        if (!fits && size_ > content_limit) {
            size_ = utf8_boundary({output_.data(), size_}, content_limit);
        }
        const auto available = fits ? value.size() : (size_ < content_limit ? content_limit - size_ : 0);
        const auto copied = fits ? value.size() : utf8_boundary(value, available);
        for (std::size_t index = 0; index < copied; ++index) {
            const auto byte = static_cast<unsigned char>(value[index]);
            output_[size_ + index] = byte <= 0x1fU || byte == 0x7fU ? ' ' : value[index];
        }
        size_ += copied;
        if (!fits) {
            std::memcpy(output_.data() + size_, marker.data(), marker_size);
            size_ += marker_size;
            truncated_ = true;
        }
    }

    template <class Value> void decimal(Value value) noexcept {
        std::array<char, 32> buffer{};
        const auto converted = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
        if (converted.ec == std::errc{}) {
            append({buffer.data(), converted.ptr});
        }
    }

    template <class Value> void hex(Value value) noexcept {
        append("0x");
        std::array<char, 2 * sizeof(Value)> buffer{};
        const auto converted = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value, 16);
        if (converted.ec == std::errc{}) {
            append({buffer.data(), converted.ptr});
        }
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return size_;
    }

  private:
    std::span<char> output_;
    std::size_t size_{};
    bool truncated_{};
};

[[nodiscard]] constexpr bool add_overflows(std::uintptr_t value, std::size_t increment) noexcept {
    return increment > std::numeric_limits<std::uintptr_t>::max() - value;
}

[[nodiscard]] bool readable_protection(DWORD protection) noexcept {
    if ((protection & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
        return false;
    }
    switch (protection & 0xffU) {
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

// Determines how much of a volatile range can be inspected without crossing an unreadable virtual memory region.
[[nodiscard]] std::size_t readable_extent(std::uintptr_t address, std::size_t maximum) noexcept {
    std::size_t total{};
    while (total < maximum && !add_overflows(address, total)) {
        const auto cursor = address + total;
        MEMORY_BASIC_INFORMATION memory{};
        if (VirtualQuery(reinterpret_cast<const void*>(cursor), &memory, sizeof(memory)) == 0 ||
            memory.State != MEM_COMMIT || !readable_protection(memory.Protect)) {
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
        total += std::min(maximum - total, static_cast<std::size_t>(region_end - cursor));
    }
    return total;
}

// This leaf has no C++ unwind state because MSVC does not permit destructible locals around a native SEH block.
[[nodiscard]] bool guarded_copy_seh(void* destination, const void* source, std::size_t size) noexcept {
    __try {
        std::memcpy(destination, source, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The scope marking a fault as expected lives outside the SEH leaf and can suppress only this thread's guarded read.
[[nodiscard]] bool guarded_copy(void* destination, const void* source, std::size_t size) noexcept {
    if (size == 0) {
        return true;
    }
    if (readable_extent(reinterpret_cast<std::uintptr_t>(source), size) != size) {
        return false;
    }
    CrashReporter::ExpectedFaultScope expected{EXCEPTION_ACCESS_VIOLATION};
    return guarded_copy_seh(destination, source, size);
}

[[nodiscard]] bool qualifying_exception(std::uint32_t code) noexcept {
    // Capture only non-customer fatal NT errors; debugger events, C++ throws, and guarded probes must pass through.
    if (code == kMsvcCppException || code == EXCEPTION_BREAKPOINT || code == EXCEPTION_SINGLE_STEP ||
        code == DBG_CONTROL_C || CrashReporter::expected_fault(code)) {
        return false;
    }
    const bool nt_error = (code & 0xc0000000U) == 0xc0000000U;
    const bool customer = (code & 0x20000000U) != 0;
    return code == kStatusFatalAppExit || (nt_error && !customer);
}

[[nodiscard]] std::string_view core_phase_name(CorePhase phase) noexcept {
    switch (phase) {
    case CorePhase::Idle:
        return "Idle";
    case CorePhase::Startup:
        return "Startup";
    case CorePhase::TargetRecognition:
        return "Target recognition";
    case CorePhase::PluginAdmission:
        return "Plugin admission";
    case CorePhase::Configuration:
        return "Configuration";
    case CorePhase::Validation:
        return "Validation";
    case CorePhase::Installation:
        return "Installation";
    case CorePhase::Running:
        return "Running";
    }
    return "Unknown";
}

[[nodiscard]] std::string_view patch_phase_name(planning::PatchPhase phase) noexcept {
    switch (phase) {
    case planning::PatchPhase::Selection:
        return "Selection";
    case planning::PatchPhase::Settings:
        return "Settings";
    case planning::PatchPhase::Create:
        return "Create";
    case planning::PatchPhase::Plan:
        return "Plan";
    case planning::PatchPhase::Validation:
        return "Validation";
    case planning::PatchPhase::Prepare:
        return "Prepare";
    case planning::PatchPhase::Commit:
        return "Commit";
    case planning::PatchPhase::Activate:
        return "Activate";
    }
    return "Unknown";
}

[[nodiscard]] std::string_view exception_name(std::uint32_t code) noexcept {
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

[[nodiscard]] std::string_view annotation_kind_name(CrashAnnotationKind kind) noexcept {
    switch (kind) {
    case CrashAnnotationKind::Plugin:
        return "Plugin";
    case CrashAnnotationKind::Callback:
        return "Callback";
    case CrashAnnotationKind::Patch:
        return "Patch";
    case CrashAnnotationKind::Mutation:
        return "Mutation";
    case CrashAnnotationKind::Hook:
        return "Hook";
    case CrashAnnotationKind::NativeResource:
        return "Native resource";
    case CrashAnnotationKind::RetainedFailure:
        return "Retained failure";
    }
    return "Unknown";
}

template <std::size_t Size> [[nodiscard]] std::string_view fixed_text(const char (&text)[Size]) noexcept {
    return {text, std::char_traits<char>::length(text)};
}

template <std::size_t Size> [[nodiscard]] std::string_view fixed_text(const std::array<char, Size>& text) noexcept {
    return {text.data(), std::char_traits<char>::length(text.data())};
}

// Bounded UTF-8 text keeps module-name conversion independent of heap allocation and temporary string lifetime.
struct FixedText {
    std::array<char, 96> bytes{};
    std::size_t size{};

    [[nodiscard]] std::string_view view() const noexcept {
        return {bytes.data(), size};
    }
};

// The no-allocation hook visitor carries the annotation owner and stable patch identity through its C callback.
struct HookPublicationContext {
    CrashReporter* reporter{};
    catalog::PatchIndex patch{};
    std::string_view patch_id;
};

// Converts a retained base address for a loaded image into bounded UTF-8 without allocating during crash publication.
[[nodiscard]] FixedText module_basename(std::uintptr_t image_base) noexcept {
    FixedText result;
    if (image_base == 0) {
        return result;
    }
    std::array<wchar_t, 32'768> path{};
    const auto length =
        GetModuleFileNameW(reinterpret_cast<HMODULE>(image_base), path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) {
        return result;
    }

    // Only the physical basename is useful in a portable report; directory paths are installation-specific noise.
    std::size_t begin{};
    for (std::size_t index = 0; index < length; ++index) {
        if (path[index] == L'\\' || path[index] == L'/') {
            begin = index + 1;
        }
    }
    auto wide_size = static_cast<int>(length - begin);
    while (wide_size > 0) {
        const auto converted =
            WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path.data() + begin, wide_size, result.bytes.data(),
                                static_cast<int>(result.bytes.size() - 1), nullptr, nullptr);
        if (converted > 0) {
            result.size = static_cast<std::size_t>(converted);
            result.bytes[result.size] = '\0';
            return result;
        }
        --wide_size;
    }
    return result;
}

// Reads only bounded PE identity fields from an already resolved loaded module.
[[nodiscard]] bool read_module_pe(HMODULE module, std::size_t image_size, CrashModuleRecord& output) noexcept {
    // Validate the DOS and fixed NT prefixes against both guarded memory access and the loader-reported image extent.
    IMAGE_DOS_HEADER dos{};
    if (!guarded_copy(&dos, module, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0) {
        return false;
    }
    const auto nt_offset = static_cast<std::size_t>(dos.e_lfanew);
    constexpr auto nt_prefix = sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER);
    if (nt_offset > image_size || nt_prefix > image_size - nt_offset) {
        return false;
    }
    const auto nt_address = reinterpret_cast<std::uintptr_t>(module) + nt_offset;
    DWORD signature{};
    IMAGE_FILE_HEADER file{};
    if (!guarded_copy(&signature, reinterpret_cast<const void*>(nt_address), sizeof(signature)) ||
        signature != IMAGE_NT_SIGNATURE ||
        !guarded_copy(&file, reinterpret_cast<const void*>(nt_address + sizeof(signature)), sizeof(file))) {
        return false;
    }
    const auto optional_offset = nt_offset + nt_prefix;
    if (file.SizeOfOptionalHeader > image_size - optional_offset) {
        return false;
    }
    const auto optional_address = nt_address + nt_prefix;
    WORD magic{};
    if (!guarded_copy(&magic, reinterpret_cast<const void*>(optional_address), sizeof(magic))) {
        return false;
    }
    // Only the preferred base differs between the supported optional headers; the timestamp comes from their COFF
    // prefix and is published after the architecture-specific read succeeds.
    if (magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
        IMAGE_OPTIONAL_HEADER32 optional{};
        if (file.SizeOfOptionalHeader < sizeof(optional) ||
            !guarded_copy(&optional, reinterpret_cast<const void*>(optional_address), sizeof(optional))) {
            return false;
        }
        output.preferred_base = optional.ImageBase;
    } else if (magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        IMAGE_OPTIONAL_HEADER64 optional{};
        if (file.SizeOfOptionalHeader < sizeof(optional) ||
            !guarded_copy(&optional, reinterpret_cast<const void*>(optional_address), sizeof(optional))) {
            return false;
        }
        output.preferred_base = optional.ImageBase;
    } else {
        return false;
    }
    output.timestamp = file.TimeDateStamp;
    return true;
}

[[nodiscard]] bool resolve_module(std::uintptr_t address, CrashModuleRecord& output) noexcept {
    // Resolve without taking a new module reference because every published code owner is already process-lifetime.
    HMODULE module{};
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(address), &module) == 0 ||
        module == nullptr) {
        return false;
    }
    MODULEINFO information{};
    if (K32GetModuleInformation(GetCurrentProcess(), module, &information, sizeof(information)) == 0 ||
        information.lpBaseOfDll == nullptr || information.SizeOfImage == 0) {
        return false;
    }
    output = {};
    output.base = reinterpret_cast<std::uintptr_t>(information.lpBaseOfDll);
    output.size = information.SizeOfImage;
    if (address < output.base || address - output.base >= output.size) {
        return false;
    }
    // A missing display name must not discard otherwise useful module and RVA facts from the crash report.
    const auto size = K32GetModuleBaseNameA(GetCurrentProcess(), module, output.basename.data(),
                                            static_cast<DWORD>(output.basename.size()));
    if (size == 0 || size >= output.basename.size()) {
        constexpr std::string_view unknown = "UnknownModule";
        std::memcpy(output.basename.data(), unknown.data(), unknown.size());
        output.basename[unknown.size()] = '\0';
    }
    static_cast<void>(read_module_pe(module, output.size, output));
    return true;
}

// Module lookup caches only the small implicated set encountered while rendering this report.
[[nodiscard]] CrashModuleRecord* find_module(CrashWorkspace& workspace, std::uintptr_t address) noexcept {
    for (std::size_t index = 0; index < workspace.module_count; ++index) {
        auto& module = workspace.modules[index];
        if (address >= module.base && address - module.base < module.size) {
            return &module;
        }
    }
    if (workspace.module_count == workspace.modules.size()) {
        return nullptr;
    }
    auto& candidate = workspace.modules[workspace.module_count];
    if (!resolve_module(address, candidate)) {
        return nullptr;
    }
    ++workspace.module_count;
    return &candidate;
}

[[nodiscard]] const CrashAnnotation* find_annotation(CrashSnapshotView snapshot, std::uintptr_t address) noexcept {
    for (const auto& annotation : snapshot.annotations) {
        if (annotation.address == 0 || address < annotation.address) {
            continue;
        }
        if ((annotation.size == 0 && address == annotation.address) ||
            (annotation.size != 0 && address - annotation.address < annotation.size)) {
            return &annotation;
        }
    }
    return nullptr;
}

// Useful addresses carry portable module/RVA and framework-owned annotation context whenever safely known.
void append_address(ReportWriter& report, CrashWorkspace& workspace, CrashSnapshotView snapshot,
                    std::uintptr_t address) noexcept {
    report.hex(address);
    if (auto* module = find_module(workspace, address); module != nullptr) {
        const auto rva = address - module->base;
        report.append(" (");
        report.append(fixed_text(module->basename));
        report.append("+");
        report.hex(rva);
        if (module->preferred_base != 0 && rva <= std::numeric_limits<std::uint64_t>::max() - module->preferred_base) {
            report.append(", preferred=");
            report.hex(module->preferred_base + rva);
        }
        report.append(")");
    }
    if (const auto* annotation = find_annotation(snapshot, address); annotation != nullptr) {
        report.append(" [");
        report.append(annotation_kind_name(annotation->kind));
        report.append(" owner=");
        report.append_structured(fixed_text(annotation->label));
        report.append("]");
    }
}

void append_hex_dump(ReportWriter& report, std::span<const std::byte> bytes, std::uintptr_t base) noexcept {
    for (std::size_t offset = 0; offset < bytes.size(); offset += 16) {
        report.append("  ");
        report.hex(base + offset);
        report.append(":");
        const auto line_size = std::min<std::size_t>(16, bytes.size() - offset);
        for (std::size_t index = 0; index < line_size; ++index) {
            report.append(" ");
            report.hex(std::to_integer<std::uint8_t>(bytes[offset + index]));
        }
        report.append("\r\n");
    }
}

#if defined(_M_X64)
// Like guarded_copy_seh, unwind SEH remains in a leaf with no C++ objects requiring destruction.
[[nodiscard]] bool unwind_once_seh(CONTEXT& context) noexcept {
    __try {
        DWORD64 image_base{};
        auto* function = RtlLookupFunctionEntry(context.Rip, &image_base, nullptr);
        if (function == nullptr) {
            std::uint64_t return_address{};
            if (!guarded_copy(&return_address, reinterpret_cast<const void*>(context.Rsp), sizeof(return_address))) {
                return false;
            }
            context.Rsp += sizeof(return_address);
            context.Rip = return_address;
            return true;
        }
        void* handler_data{};
        DWORD64 establisher_frame{};
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, context.Rip, function, &context, &handler_data,
                         &establisher_frame, nullptr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

[[nodiscard]] bool unwind_once(CONTEXT& context) noexcept {
    CrashReporter::ExpectedFaultScope expected{EXCEPTION_ACCESS_VIOLATION};
    return unwind_once_seh(context);
}
#endif

// Architecture-native unwinding provides the primary bounded frame list before the looser stack candidate scan.
void append_stack_frames(ReportWriter& report, CrashWorkspace& workspace, CrashSnapshotView snapshot,
                         const CONTEXT& context) noexcept {
    report.append("\r\nStack frames:\r\n");
#if defined(_M_X64)
    // x64 unwind metadata advances a private context copy and stops if malformed state makes no progress.
    workspace.unwind_context = context;
    std::size_t count{};
    while (count < kMaximumStackFrames && workspace.unwind_context.Rip != 0) {
        report.append("  #");
        report.decimal(count);
        report.append(" ");
        append_address(report, workspace, snapshot, workspace.unwind_context.Rip);
        report.append("\r\n");
        ++count;
        const auto previous_ip = workspace.unwind_context.Rip;
        const auto previous_sp = workspace.unwind_context.Rsp;
        if (!unwind_once(workspace.unwind_context) ||
            (workspace.unwind_context.Rip == previous_ip && workspace.unwind_context.Rsp == previous_sp)) {
            break;
        }
    }
#else
    // x86 has no equivalent table-driven unwind, so follow only a strictly increasing guarded frame chain.
    report.append("  #0 ");
    append_address(report, workspace, snapshot, context.Eip);
    report.append("\r\n");
    struct Frame {
        std::uint32_t next;
        std::uint32_t return_address;
    };
    auto frame_pointer = static_cast<std::uintptr_t>(context.Ebp);
    for (std::size_t index = 1; index < kMaximumStackFrames; ++index) {
        Frame frame{};
        if (!guarded_copy(&frame, reinterpret_cast<const void*>(frame_pointer), sizeof(frame)) ||
            frame.next <= frame_pointer || frame.return_address == 0) {
            break;
        }
        report.append("  #");
        report.decimal(index);
        report.append(" ");
        append_address(report, workspace, snapshot, frame.return_address);
        report.append("\r\n");
        frame_pointer = frame.next;
    }
#endif
}

// Fixed instruction and stack windows preserve offline evidence without DbgHelp, allocation, or unbounded walking.
void append_memory_windows(ReportWriter& report, CrashWorkspace& workspace, CrashSnapshotView snapshot,
                           std::uintptr_t instruction_pointer, std::uintptr_t stack_pointer) noexcept {
    // Capture the readable instruction prefix independently so an invalid stack does not hide fault-site bytes.
    const auto instruction_size = readable_extent(instruction_pointer, workspace.instruction_window.size());
    report.append("\r\nInstruction bytes:\r\n");
    if (instruction_size == 0 || !guarded_copy(workspace.instruction_window.data(),
                                               reinterpret_cast<const void*>(instruction_pointer), instruction_size)) {
        report.append("  (unavailable)\r\n");
    } else {
        append_hex_dump(report, std::span{workspace.instruction_window}.first(instruction_size), instruction_pointer);
    }

    // The larger stack copy supplies both the displayed prefix and a bounded scan for implicated addresses.
    const auto scan_size = readable_extent(stack_pointer, workspace.stack_scan.size());
    const auto stack_copied = scan_size != 0 && guarded_copy(workspace.stack_scan.data(),
                                                             reinterpret_cast<const void*>(stack_pointer), scan_size);
    report.append("\r\nStack window:\r\n");
    if (!stack_copied) {
        report.append("  (unavailable)\r\n");
    } else {
        append_hex_dump(report, std::span{workspace.stack_scan}.first(std::min(scan_size, kStackWindowSize)),
                        stack_pointer);
    }

    // Report only words that map to a loaded image or published annotation; arbitrary stack data adds no attribution.
    report.append("\r\nAnnotated stack candidates:\r\n");
    std::size_t candidates{};
    if (stack_copied) {
        for (std::size_t offset = 0;
             offset + sizeof(std::uintptr_t) <= scan_size && candidates < kMaximumStackCandidates;
             offset += sizeof(std::uintptr_t)) {
            std::uintptr_t value{};
            std::memcpy(&value, workspace.stack_scan.data() + offset, sizeof(value));
            if (find_module(workspace, value) == nullptr && find_annotation(snapshot, value) == nullptr) {
                continue;
            }
            report.append("  [SP+");
            report.hex(offset);
            report.append("] ");
            append_address(report, workspace, snapshot, value);
            report.append("\r\n");
            ++candidates;
        }
    }
    if (candidates == 0) {
        report.append("  (none)\r\n");
    }
}

void append_modules(ReportWriter& report, const CrashWorkspace& workspace) noexcept {
    report.append("\r\nImplicated modules:\r\n");
    if (workspace.module_count == 0) {
        report.append("  (none)\r\n");
        return;
    }
    for (std::size_t index = 0; index < workspace.module_count; ++index) {
        const auto& module = workspace.modules[index];
        report.append("  ");
        report.append_structured(fixed_text(module.basename));
        report.append(" base=");
        report.hex(module.base);
        report.append(" size=");
        report.hex(module.size);
        report.append(" timestamp=");
        report.hex(module.timestamp);
        report.append("\r\n");
    }
}

// Returns the validated native address without trusting an unchecked RVA from the patch plan during publication.
[[nodiscard]] std::uintptr_t native_address(const targets::RecognizedTarget& target, FC_TargetImage image,
                                            std::uint32_t rva) noexcept {
    const auto* view = target.find(image);
    if (view == nullptr || rva >= view->info().size ||
        view->info().base > std::numeric_limits<std::uintptr_t>::max() - rva) {
        return 0;
    }
    return view->info().base + rva;
}

// Mutation size comes from the frozen claim generated for the same planned operation.
[[nodiscard]] std::uint64_t mutation_size(const planning::SubmittedPlan& plan, std::uint32_t operation_index) noexcept {
    const auto found = std::ranges::find_if(plan.claims, [&](const planning::MemoryClaim& claim) {
        return claim.operation_index == operation_index && claim.access == planning::ClaimAccess::Write;
    });
    return found == plan.claims.end() ? 0 : found->size;
}

// Normalizes distinct native callback pointer types into one report address without invoking plugin code.
template <class Function> [[nodiscard]] std::uintptr_t callback_address(Function callback) noexcept {
    return reinterpret_cast<std::uintptr_t>(callback);
}

// Gives each of the three mutation variants a stable operation name for crash reports.
[[nodiscard]] std::string_view operation_label(const planning::OperationRecord& operation) noexcept {
    return std::visit(
        [](const auto& payload) -> std::string_view {
            using Operation = std::remove_cvref_t<decltype(payload)>;
            if constexpr (std::same_as<Operation, planning::WriteOperation>) {
                return "Native write";
            } else if constexpr (std::same_as<Operation, planning::NopOperation>) {
                return "Native NOP";
            } else if constexpr (std::same_as<Operation, planning::RedirectOperation>) {
                return "Native redirect";
            } else {
                return {};
            }
        },
        operation.payload);
}

} // namespace

static_assert(std::atomic<std::uint32_t>::is_always_lock_free,
              "Crash snapshot cursors require lock-free 32-bit atomics on supported targets");
static_assert(std::atomic<CrashReporter*>::is_always_lock_free,
              "Crash handler publication requires lock-free pointers on supported targets");

std::atomic<CrashReporter*> CrashReporter::active_{};

CrashReporter::CrashReporter(CrashPhaseCursors& cursors) noexcept : cursors_(&cursors) {
    LARGE_INTEGER started{};
    QueryPerformanceCounter(&started);
    process_started_ = static_cast<std::uint64_t>(started.QuadPart);
}

CrashReporter::~CrashReporter() {
    if (handler_ != nullptr) {
        RemoveVectoredExceptionHandler(handler_);
        handler_ = nullptr;
    }
    auto* expected = this;
    active_.compare_exchange_strong(expected, nullptr, std::memory_order_release, std::memory_order_relaxed);
    delete[] annotations_;
    delete workspace_;
}

bool CrashReporter::install(const std::filesystem::path& output_directory) noexcept {
    if (annotations_ != nullptr && workspace_ != nullptr && handler_ != nullptr) {
        return true;
    }
    // Allocate every variable-sized crash resource before registration; capture itself must remain heap-independent.
    annotations_ = new (std::nothrow) CrashAnnotation[kAnnotationCapacity];
    workspace_ = new (std::nothrow) CrashWorkspace;
    if (annotations_ == nullptr || workspace_ == nullptr) {
        delete[] annotations_;
        annotations_ = nullptr;
        delete workspace_;
        workspace_ = nullptr;
        return false;
    }

    // The exceptional path cannot allocate a filesystem path, so resolve the final adjacent filename now.
    constexpr std::wstring_view crash_name = L"FusionCutter.Crash.log";
    std::size_t filename{};
    bool path_ready{};
    if (!output_directory.empty()) {
        const auto requested = (output_directory / crash_name).native();
        if (requested.size() + 1 <= report_path_.size()) {
            std::memcpy(report_path_.data(), requested.data(), requested.size() * sizeof(wchar_t));
            report_path_[requested.size()] = L'\0';
            filename = requested.size() - crash_name.size();
            path_ready = true;
        }
    } else {
        HMODULE module{};
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(&CrashReporter::vectored_handler), &module) != 0 &&
            module != nullptr) {
            const auto length =
                GetModuleFileNameW(module, report_path_.data(), static_cast<DWORD>(report_path_.size()));
            if (length != 0 && length < report_path_.size()) {
                path_ready = true;
                for (std::size_t index = 0; index < length; ++index) {
                    if (report_path_[index] == L'\\' || report_path_[index] == L'/') {
                        filename = index + 1;
                    }
                }
            }
        }
    }
    if (!path_ready || filename + crash_name.size() + 1 > report_path_.size()) {
        delete[] annotations_;
        annotations_ = nullptr;
        delete workspace_;
        workspace_ = nullptr;
        return false;
    }
    std::memcpy(report_path_.data() + filename, crash_name.data(), crash_name.size() * sizeof(wchar_t));
    report_path_[filename + crash_name.size()] = L'\0';

    // Cache process identity now because loader and filesystem queries are unsafe assumptions on a damaged thread.
    std::array<wchar_t, 32'768> executable_path{};
    const auto executable_length =
        GetModuleFileNameW(nullptr, executable_path.data(), static_cast<DWORD>(executable_path.size()));
    if (executable_length != 0 && executable_length < executable_path.size()) {
        std::size_t executable_filename{};
        for (std::size_t index = 0; index < executable_length; ++index) {
            if (executable_path[index] == L'\\' || executable_path[index] == L'/') {
                executable_filename = index + 1;
            }
        }
        const auto wide_size = static_cast<int>(executable_length - executable_filename);
        const auto converted = WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, executable_path.data() + executable_filename, wide_size,
            executable_basename_.data(), static_cast<int>(executable_basename_.size() - 1), nullptr, nullptr);
        if (converted > 0) {
            executable_basename_[static_cast<std::size_t>(converted)] = '\0';
        }
    }

    // Facts read during capture must be immutable before the handler becomes visible to another thread.
    FILETIME creation{};
    FILETIME exit{};
    FILETIME kernel{};
    FILETIME user{};
    if (GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user) != 0) {
        ULARGE_INTEGER value{};
        value.LowPart = creation.dwLowDateTime;
        value.HighPart = creation.dwHighDateTime;
        process_creation_time_ = value.QuadPart;
    }
    using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    if (const auto ntdll = GetModuleHandleW(L"ntdll.dll"); ntdll != nullptr) {
        const auto get_version = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"));
        RTL_OSVERSIONINFOW version{};
        version.dwOSVersionInfoSize = sizeof(version);
        if (get_version != nullptr && get_version(&version) == 0) {
            windows_major_ = version.dwMajorVersion;
            windows_minor_ = version.dwMinorVersion;
            windows_build_ = version.dwBuildNumber;
        }
    }

    // Publish a single active owner before registering the handler that may immediately observe it on another thread.
    CrashReporter* expected{};
    if (!active_.compare_exchange_strong(expected, this, std::memory_order_release, std::memory_order_relaxed)) {
        delete[] annotations_;
        annotations_ = nullptr;
        delete workspace_;
        workspace_ = nullptr;
        return false;
    }
    handler_ = AddVectoredExceptionHandler(1, &CrashReporter::vectored_handler);
    if (handler_ == nullptr) {
        active_.store(nullptr, std::memory_order_release);
        delete[] annotations_;
        annotations_ = nullptr;
        delete workspace_;
        workspace_ = nullptr;
        return false;
    }

    return true;
}

void CrashReporter::set_session(FC_HostRole role, std::string_view selected_proxy_basename) noexcept {
    session_role_ = role;
    copy_structured(selected_proxy_basename_, selected_proxy_basename);
}

void CrashReporter::set_core_phase(CorePhase phase) noexcept {
    cursors_->core_phase.store(static_cast<std::uint32_t>(phase), std::memory_order_release);
}

void CrashReporter::set_current_patch(catalog::PatchIndex patch, planning::PatchPhase phase) noexcept {
    cursors_->current_patch_phase.store(static_cast<std::uint32_t>(phase), std::memory_order_relaxed);
    cursors_->current_patch.store(patch.value, std::memory_order_release);
}

void CrashReporter::set_patch_phase(planning::PatchPhase phase) noexcept {
    cursors_->current_patch_phase.store(static_cast<std::uint32_t>(phase), std::memory_order_release);
}

void CrashReporter::clear_current_patch() noexcept {
    cursors_->current_patch.store(std::numeric_limits<std::uint32_t>::max(), std::memory_order_release);
}

void CrashReporter::set_target(const targets::RecognizedTarget& target) noexcept {
    target_layout_ = target.layout();
    target_role_ = target.role();
    target_architecture_ = target.architecture();
    const auto* image = target.find(FC_IMAGE_GAME);
    if (image == nullptr) {
        image = target.find(FC_IMAGE_BOOTSTRAP);
    }
    if (image != nullptr) {
        copy_structured(target_profile_, image->info().profile);
    }
    target_ready_.store(true, std::memory_order_release);
}

void CrashReporter::append(CrashAnnotationKind kind, std::uint32_t patch, std::uintptr_t address, std::uint64_t size,
                           std::string_view label, std::string_view detail, std::string_view module,
                           std::uint32_t timestamp) noexcept {
    if (annotations_ == nullptr) {
        return;
    }
    // Overflow never compromises earlier evidence; it increments a separate saturating scalar visible to capture.
    const auto index = published_count_.load(std::memory_order_relaxed);
    if (index >= kAnnotationCapacity) {
        auto omitted = omitted_count_.load(std::memory_order_relaxed);
        while (omitted != std::numeric_limits<std::uint32_t>::max() &&
               !omitted_count_.compare_exchange_weak(omitted, omitted + 1, std::memory_order_relaxed)) {
            // A failed compare-exchange refreshes omitted for the next saturating attempt.
        }
        return;
    }

    // Construct the complete fixed record locally before the single normal writer touches unpublished array storage.
    CrashAnnotation record{.kind = kind, .patch = patch, .address = address, .size = size, .timestamp = timestamp};
    copy_structured(record.label, label);
    copy_structured(record.detail, detail);
    copy_structured(record.module, module);
    annotations_[index] = record;
    // The release publishes the completely initialized immutable slot to a concurrent exceptional reader.
    published_count_.store(index + 1, std::memory_order_release);
}

void CrashReporter::publish_catalog(const catalog::Catalog& catalog) noexcept {
    // Essential admitted plugin/module identities are published first so later annotation overflow cannot omit them.
    for (const auto& plugin : catalog.plugins()) {
        const auto extent = plugin.code_owner.image_extent();
        const auto basename = module_basename(extent.begin);
        append(CrashAnnotationKind::Plugin, std::numeric_limits<std::uint32_t>::max(), extent.begin,
               extent.end - extent.begin, plugin.definition.id, plugin.definition.version, basename.view(),
               plugin.code_owner.timestamp());
    }

    // Callback addresses retain patch ownership without consulting the plugin catalog or invoking code during a fault.
    struct CallbackFact {
        std::string_view name;
        std::uintptr_t address;
    };
    for (std::uint32_t value = 0; value < catalog.patch_count(); ++value) {
        const catalog::PatchIndex patch{value};
        const auto& definition = catalog.patch(patch);
        if (!definition.selected_support) {
            continue;
        }
        const auto& callbacks = definition.supports[*definition.selected_support].callbacks;
        const std::array facts{
            CallbackFact{"Create", callback_address(callbacks.create)},
            CallbackFact{"Destroy", callback_address(callbacks.destroy)},
            CallbackFact{"Plan", callback_address(callbacks.plan)},
            CallbackFact{"Prepare", callback_address(callbacks.prepare)},
            CallbackFact{"Activate", callback_address(callbacks.activate)},
            CallbackFact{"Update", callback_address(callbacks.update)},
            CallbackFact{"WriteStatus", callback_address(callbacks.write_status)},
            CallbackFact{"QueryInterface", callback_address(callbacks.query_interface)},
        };
        const auto& owner = catalog.plugin(catalog.patch_plugin(patch)).code_owner;
        const auto extent = owner.image_extent();
        const auto basename = module_basename(extent.begin);
        for (const auto& fact : facts) {
            if (fact.address != 0) {
                append(CrashAnnotationKind::Callback, patch.value, fact.address, 0, definition.id, fact.name,
                       basename.view(), owner.timestamp());
            }
        }
    }
}

void CrashReporter::publish_installed_patch(catalog::PatchIndex patch, const catalog::Catalog& catalog,
                                            const targets::RecognizedTarget& target,
                                            const planning::SubmittedPlan& plan,
                                            const patching::NativePatchResources& resources,
                                            const patching::HookRegistry& hooks) noexcept {
    const auto& definition = catalog.patch(patch);
    const auto image = definition.supports[*definition.selected_support].image;
    append(CrashAnnotationKind::Patch, patch.value, 0, 0, definition.id, definition.version);

    // Copy native sites and participant thunks while their frozen patch plan still provides ownership context.
    for (const auto& operation : plan.operations) {
        if (const auto* hook = std::get_if<planning::HookOperation>(&operation.payload)) {
            append(CrashAnnotationKind::Hook, patch.value, native_address(target, image, hook->location.rva),
                   hook->overwrite_size, definition.id, hook->observer ? "Shared hook observer" : "Shared hook owner");
            if (hook->callback != 0) {
                append(CrashAnnotationKind::Callback, patch.value, hook->callback, 0, definition.id,
                       hook->observer ? "Hook before callback" : "Hook owner callback");
            }
            if (hook->after != 0) {
                append(CrashAnnotationKind::Callback, patch.value, hook->after, 0, definition.id,
                       "Hook after callback");
            }
            continue;
        }
        if (const auto* binding = std::get_if<planning::InterfaceBindingOperation>(&operation.payload)) {
            append(CrashAnnotationKind::Callback, patch.value, callback_address(binding->connect), 0, definition.id,
                   "Interface connection callback");
            continue;
        }

        const auto label = operation_label(operation);
        std::uint32_t rva{};
        bool mutation{};
        std::visit(
            [&](const auto& payload) {
                using Operation = std::remove_cvref_t<decltype(payload)>;
                if constexpr (std::same_as<Operation, planning::WriteOperation> ||
                              std::same_as<Operation, planning::NopOperation> ||
                              std::same_as<Operation, planning::RedirectOperation>) {
                    rva = payload.location.rva;
                    mutation = true;
                }
            },
            operation.payload);
        if (mutation) {
            append(CrashAnnotationKind::Mutation, patch.value, native_address(target, image, rva),
                   mutation_size(plan, operation.index), definition.id, label);
        }
    }

    // Dynamic allocations and physical hook entry/trampoline ranges have no durable representation outside owners.
    publish_native_resources(patch, definition.id, resources);
    HookPublicationContext context{this, patch, definition.id};
    hooks.visit_patch_resources(patch, &context, &publish_hook_resource_callback);
}

void CrashReporter::publish_retained_failure(catalog::PatchIndex patch, patching::RollbackResult rollback,
                                             std::span<const planning::MemoryClaim> blocked_claims,
                                             std::string_view patch_id, const targets::RecognizedTarget& target,
                                             const patching::NativePatchResources& resources,
                                             const patching::HookPreparation& hooks) noexcept {
    const auto detail = rollback == patching::RollbackResult::Residual ? "Residual rollback" : "Restored exposed state";
    append(CrashAnnotationKind::RetainedFailure, patch.value, 0, blocked_claims.size(), patch_id, detail);
    // Blockers use actual target addresses so capture never needs a live target lookup or image-relative calculation.
    for (const auto& claim : blocked_claims) {
        append(CrashAnnotationKind::NativeResource, patch.value, native_address(target, claim.image, claim.rva),
               claim.size, patch_id, "Blocked retained native range");
    }
    publish_native_resources(patch, patch_id, resources);
    HookPublicationContext context{this, patch, patch_id};
    hooks.visit_resources(&context, &publish_hook_resource_callback);
}

void CrashReporter::publish_native_resources(catalog::PatchIndex patch, std::string_view patch_id,
                                             const patching::NativePatchResources& resources) noexcept {
    for (const auto& data : resources.data_allocations) {
        append(CrashAnnotationKind::NativeResource, patch.value, data.allocation.address(), data.allocation.size(),
               patch_id, "Patch data allocation");
    }
    for (const auto& relay : resources.relay_allocations) {
        append(CrashAnnotationKind::NativeResource, patch.value, relay.address(), relay.size(), patch_id,
               "Executable relay allocation");
    }
}

void CrashReporter::publish_hook_resource(catalog::PatchIndex patch, std::string_view patch_id,
                                          const patching::HookResourceView& resource) noexcept {
    if (resource.entry_address != 0 && resource.entry_size != 0) {
        append(CrashAnnotationKind::NativeResource, patch.value, resource.entry_address, resource.entry_size, patch_id,
               "Executable hook entry allocation");
    }
    if (resource.trampoline_address != 0) {
        append(CrashAnnotationKind::NativeResource, patch.value, resource.trampoline_address, 0, patch_id,
               "Executable hook trampoline");
    }
}

void CrashReporter::publish_hook_resource_callback(void* context, const patching::HookResourceView& resource) noexcept {
    auto& publication = *static_cast<HookPublicationContext*>(context);
    publication.reporter->publish_hook_resource(publication.patch, publication.patch_id, resource);
}

CrashSnapshotView CrashReporter::snapshot() const noexcept {
    // Acquire the completed prefix once; records below it were published with release semantics and cannot change.
    const auto count = published_count_.load(std::memory_order_acquire);
    const auto current = cursors_->current_patch.load(std::memory_order_acquire);
    return {.annotations = annotations_ == nullptr ? std::span<const CrashAnnotation>{}
                                                   : std::span<const CrashAnnotation>{annotations_, count},
            .omitted = omitted_count_.load(std::memory_order_relaxed),
            .core_phase = static_cast<CorePhase>(cursors_->core_phase.load(std::memory_order_acquire)),
            .current_patch = catalog::PatchIndex{current},
            .current_patch_phase =
                static_cast<planning::PatchPhase>(cursors_->current_patch_phase.load(std::memory_order_acquire)),
            .has_current_patch = current != std::numeric_limits<std::uint32_t>::max()};
}

long __stdcall CrashReporter::vectored_handler(_EXCEPTION_POINTERS* exception) noexcept {
    auto* reporter = active_.load(std::memory_order_acquire);
    return reporter == nullptr ? EXCEPTION_CONTINUE_SEARCH : reporter->capture(exception);
}

long CrashReporter::capture(_EXCEPTION_POINTERS* exception) noexcept {
    // Native structure validation and filtering happen before claiming the single exceptional workspace.
    if (exception == nullptr || exception->ExceptionRecord == nullptr || exception->ContextRecord == nullptr) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const auto code = exception->ExceptionRecord->ExceptionCode;
    if (!qualifying_exception(code) || handling_.test_and_set(std::memory_order_acq_rel)) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    // Qualifying attempts consume their fixed slot before I/O so a failing disk cannot cause unbounded retries.
    auto attempt = report_attempts_.load(std::memory_order_relaxed);
    while (attempt < kMaximumCrashAttempts &&
           !report_attempts_.compare_exchange_weak(attempt, attempt + 1, std::memory_order_relaxed)) {
        // compare_exchange refreshes attempt for the saturating retry.
    }
    if (attempt >= kMaximumCrashAttempts) {
        handling_.clear(std::memory_order_release);
        return EXCEPTION_CONTINUE_SEARCH;
    }

    if (workspace_ == nullptr) {
        handling_.clear(std::memory_order_release);
        return EXCEPTION_CONTINUE_SEARCH;
    }
    workspace_->module_count = 0;
    ReportWriter report{workspace_->report};
    const auto captured = snapshot();
    // Begin with build, process, and session identity so even a truncated report remains attributable.
    SYSTEMTIME utc{};
    GetSystemTime(&utc);
    LARGE_INTEGER now{};
    LARGE_INTEGER frequency{};
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    report.append("Fusion Cutter first-chance crash capture\r\nReport: ");
    report.decimal(attempt + 1);
    report.append(" of 4\r\nUTC: ");
    report.decimal(utc.wYear);
    report.append("-");
    report.decimal(utc.wMonth);
    report.append("-");
    report.decimal(utc.wDay);
    report.append(" ");
    report.decimal(utc.wHour);
    report.append(":");
    report.decimal(utc.wMinute);
    report.append(":");
    report.decimal(utc.wSecond);
    report.append(".");
    report.decimal(utc.wMilliseconds);
    report.append("Z\r\nUptime QPC ticks: ");
    report.decimal(static_cast<std::uint64_t>(now.QuadPart) - process_started_);
    report.append(" / ");
    report.decimal(frequency.QuadPart);
    report.append(" per second\r\nProcess uptime ms: ");
    FILETIME current_time{};
    GetSystemTimeAsFileTime(&current_time);
    ULARGE_INTEGER current_value{};
    current_value.LowPart = current_time.dwLowDateTime;
    current_value.HighPart = current_time.dwHighDateTime;
    report.decimal(process_creation_time_ != 0 && current_value.QuadPart >= process_creation_time_
                       ? (current_value.QuadPart - process_creation_time_) / 10'000U
                       : 0);
    report.append("\r\nPID: ");
    report.decimal(GetCurrentProcessId());
    report.append("\r\nThread: ");
    report.decimal(GetCurrentThreadId());
    report.append("\r\nFusion Cutter: ");
    report.append(FC_VERSION_STRING);
    report.append(" (build ");
    report.append(FC_BUILD_ID);
    report.append(")\r\nWindows: ");
    report.decimal(windows_major_);
    report.append(".");
    report.decimal(windows_minor_);
    report.append(" build ");
    report.decimal(windows_build_);
    report.append("\r\nExecutable: ");
    report.append_structured(
        {executable_basename_.data(), std::char_traits<char>::length(executable_basename_.data())});
    report.append("\r\nHost role: ");
    report.append(session_role_ == FC_HOST_ROLE_SERVER ? "Server" : "Client");
    report.append("\r\nDirectInput proxy: ");
    report.append_structured(selected_proxy_basename_[0] == '\0' ? std::string_view{"None"}
                                                                 : fixed_text(selected_proxy_basename_));
    report.append("\r\n");

    if (target_ready_.load(std::memory_order_acquire)) {
        report.append("Target tuple: layout=");
        report.decimal(target_layout_);
        report.append(", role=");
        report.decimal(target_role_);
        report.append(", architecture=");
        report.decimal(target_architecture_);
        report.append(", profile=");
        report.append_structured({target_profile_.data(), std::char_traits<char>::length(target_profile_.data())});
        report.append("\r\n");
    } else {
        report.append("Target: Unknown\r\n");
    }

    // Phase cursors connect the fault to the last framework and patch lifecycle transitions without runtime locks.
    report.append("Framework phase: ");
    report.append(core_phase_name(captured.core_phase));
    report.append("\r\nCurrent patch: ");
    if (captured.has_current_patch) {
        report.decimal(captured.current_patch.value);
        for (const auto& annotation : captured.annotations) {
            if (annotation.patch == captured.current_patch.value && annotation.label[0] != '\0') {
                report.append(" /");
                report.append_structured(fixed_text(annotation.label));
                break;
            }
        }
        report.append(" (");
        report.append(patch_phase_name(captured.current_patch_phase));
        report.append(")\r\n");
    } else {
        report.append("None\r\n");
    }

    // Exception semantics precede raw registers so the most actionable fault facts survive bounded truncation.
    const auto* record = exception->ExceptionRecord;
    report.append("\r\nException code: ");
    report.hex(code);
    report.append(" (");
    report.append(exception_name(code));
    report.append(")");
    report.append("\r\nFlags: ");
    report.hex(record->ExceptionFlags);
    report.append("\r\nInstruction address: ");
    append_address(report, *workspace_, captured, reinterpret_cast<std::uintptr_t>(record->ExceptionAddress));
    report.append("\r\nParameters:");
    for (DWORD index = 0; index < record->NumberParameters; ++index) {
        report.append(" ");
        report.hex(record->ExceptionInformation[index]);
    }
    report.append("\r\n");
    if ((code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR) && record->NumberParameters >= 2) {
        report.append("Access violation: ");
        report.append(record->ExceptionInformation[0] == 0   ? "read"
                      : record->ExceptionInformation[0] == 1 ? "write"
                                                             : "execute");
        report.append(" at ");
        append_address(report, *workspace_, captured, record->ExceptionInformation[1]);
        report.append("\r\n");
    }

    // Architecture-specific volatile context includes argument registers commonly needed to diagnose hook failures.
    const auto* context = exception->ContextRecord;
#if defined(_M_X64)
    const auto instruction_pointer = static_cast<std::uintptr_t>(context->Rip);
    const auto stack_pointer = static_cast<std::uintptr_t>(context->Rsp);
    report.append("RIP: ");
    report.hex(context->Rip);
    report.append(" RSP: ");
    report.hex(context->Rsp);
    report.append(" RBP: ");
    report.hex(context->Rbp);
    report.append(" RFLAGS: ");
    report.hex(context->EFlags);
    report.append("\r\nRAX: ");
    report.hex(context->Rax);
    report.append(" RBX: ");
    report.hex(context->Rbx);
    report.append(" RCX: ");
    report.hex(context->Rcx);
    report.append(" RDX: ");
    report.hex(context->Rdx);
    report.append("\r\nRSI: ");
    report.hex(context->Rsi);
    report.append(" RDI: ");
    report.hex(context->Rdi);
    report.append(" R8: ");
    report.hex(context->R8);
    report.append(" R9: ");
    report.hex(context->R9);
    report.append("\r\nR10: ");
    report.hex(context->R10);
    report.append(" R11: ");
    report.hex(context->R11);
    report.append(" R12: ");
    report.hex(context->R12);
    report.append(" R13: ");
    report.hex(context->R13);
    report.append("\r\nR14: ");
    report.hex(context->R14);
    report.append(" R15: ");
    report.hex(context->R15);
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
    report.append("EIP: ");
    report.hex(context->Eip);
    report.append(" ESP: ");
    report.hex(context->Esp);
    report.append(" EBP: ");
    report.hex(context->Ebp);
    report.append(" EFLAGS: ");
    report.hex(context->EFlags);
    report.append("\r\nEAX: ");
    report.hex(context->Eax);
    report.append(" EBX: ");
    report.hex(context->Ebx);
    report.append(" ECX: ");
    report.hex(context->Ecx);
    report.append(" EDX: ");
    report.hex(context->Edx);
    report.append(" ESI: ");
    report.hex(context->Esi);
    report.append(" EDI: ");
    report.hex(context->Edi);
    report.append("\r\n");
#endif

    // Bounded unwind and memory evidence use guarded reads; failures omit evidence rather than recurse into capture.
    append_stack_frames(report, *workspace_, captured, *context);
    append_memory_windows(report, *workspace_, captured, instruction_pointer, stack_pointer);

    // Immutable annotations relate addresses back to admitted modules, patch plans, allocations, and hook resources.
    report.append("\r\nPublished crash annotations: ");
    report.decimal(captured.annotations.size());
    report.append(" (omitted ");
    report.decimal(captured.omitted);
    report.append(")\r\n");
    for (const auto& annotation : captured.annotations) {
        report.append("  [");
        report.append(annotation_kind_name(annotation.kind));
        report.append("] ");
        report.append_structured(fixed_text(annotation.label));
        if (annotation.detail[0] != '\0') {
            report.append(" | ");
            report.append_structured(fixed_text(annotation.detail));
        }
        if (annotation.module[0] != '\0') {
            report.append(" | module=");
            report.append_structured(fixed_text(annotation.module));
        }
        if (annotation.address != 0) {
            report.append(" | address=");
            append_address(report, *workspace_, captured, annotation.address);
        }
        if (annotation.size != 0) {
            report.append(" | size=");
            report.decimal(annotation.size);
        }
        report.append("\r\n");
    }
    append_modules(report, *workspace_);
    report.append("\r\nThis is first-chance capture; Fusion Cutter does not claim the process terminated.\r\n");

    // The first successful attempt replaces stale process output; later successful attempts append up to the fixed cap.
    const bool append = file_started_.load(std::memory_order_relaxed);
    const auto file = CreateFileW(
        report_path_.data(), FILE_APPEND_DATA | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, append ? OPEN_ALWAYS : CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
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
        if (io_error == ERROR_SUCCESS &&
            WriteFile(file, workspace_->report.data(), static_cast<DWORD>(report.size()), &written, nullptr) != 0 &&
            written == report.size() && FlushFileBuffers(file) != 0) {
            success = true;
        } else if (io_error == ERROR_SUCCESS) {
            io_error = GetLastError();
        }
        CloseHandle(file);
    } else {
        io_error = GetLastError();
    }
    if (success) {
        file_started_.store(true, std::memory_order_relaxed);
    } else {
        std::array<char, 256> fallback{};
        ReportWriter debug{fallback};
        debug.append("Fusion Cutter crash capture file I/O failed; code=");
        debug.hex(code);
        debug.append(", address=");
        debug.hex(reinterpret_cast<std::uintptr_t>(record->ExceptionAddress));
        debug.append(", Windows error=");
        debug.decimal(io_error);
        debug.append("\r\n");
        if (debug.size() < fallback.size()) {
            fallback[debug.size()] = '\0';
        } else {
            fallback.back() = '\0';
        }
        OutputDebugStringA(fallback.data());
    }

    handling_.clear(std::memory_order_release);
    return EXCEPTION_CONTINUE_SEARCH;
}

CrashReporter::ExpectedFaultScope::ExpectedFaultScope(std::uint32_t exception_code) noexcept
    : previous_(std::exchange(expected_exception_code, exception_code)) {}

CrashReporter::ExpectedFaultScope::~ExpectedFaultScope() {
    expected_exception_code = previous_;
}

bool CrashReporter::expected_fault(std::uint32_t exception_code) noexcept {
    return expected_exception_code != 0 && expected_exception_code == exception_code;
}

} // namespace fc::runtime
