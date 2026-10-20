#include "crash_report.hpp"

#include <Psapi.h>

namespace fc::runtime {

namespace crash_detail {

namespace {

// Returns the report label for an annotation kind, using Unknown for unrecognized values.
std::string_view annotation_kind_name(CrashAnnotationKind kind) noexcept {
    switch (kind) {
    case CrashAnnotationKind::Plugin:
        return "Plugin";
    case CrashAnnotationKind::Patch:
        return "Patch";
    case CrashAnnotationKind::Mutation:
        return "Mutation";
    case CrashAnnotationKind::Hook:
        return "Hook";
    case CrashAnnotationKind::NativeResource:
        return "Native resource";
    case CrashAnnotationKind::Module:
        return "Module";
    }
    return "Unknown";
}

} // namespace

bool resolve_module(std::uintptr_t address, CrashModuleRecord &output) noexcept {
    // Ask Windows which loaded module contains the address; its current mapping supplies the bounds and name.
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

    // Keep the module bounds useful even when its display name is unavailable.
    const auto size =
        K32GetModuleBaseNameA(GetCurrentProcess(), module, output.basename.data(), static_cast<DWORD>(output.basename.size()));
    if (size == 0 || size >= output.basename.size()) {
        constexpr std::string_view unknown = "UnknownModule";
        std::memcpy(output.basename.data(), unknown.data(), unknown.size());
        output.basename[unknown.size()] = '\0';
    }

    return true;
}

namespace {

// Finds a containing plugin or module in newest-batch order; the result borrows the retained batch storage.
const CrashAnnotation *find_module_annotation(const CrashAnnotationBatch *facts, std::uintptr_t address) noexcept {
    for (auto *batch = facts; batch; batch = batch->next) {
        for (const auto &annotation : batch->records) {
            if ((annotation.kind == CrashAnnotationKind::Plugin || annotation.kind == CrashAnnotationKind::Module) && annotation.address &&
                address >= annotation.address && address - annotation.address < annotation.size) {
                return &annotation;
            }
        }
    }
    return nullptr;
}

// Finds the most specific recorded address or range, retaining the newest match when specificity is equal.
const CrashAnnotation *find_annotation(const CrashAnnotationBatch *facts, std::uintptr_t address) noexcept {
    const CrashAnnotation *best{};
    for (auto *batch = facts; batch; batch = batch->next) {
        for (const auto &annotation : batch->records) {
            if (annotation.address == 0 || address < annotation.address) {
                continue;
            }
            const bool matches = (annotation.size == 0 && address == annotation.address) ||
                                 (annotation.size != 0 && address - annotation.address < annotation.size);
            if (!matches) {
                continue;
            }

            // Prefer exact addresses, then smaller ranges; on equal extents prefer another kind over Plugin.
            if (best == nullptr || (annotation.size == 0 && best->size != 0) ||
                (annotation.size != 0 && best->size != 0 && annotation.size < best->size) ||
                (annotation.size == best->size && best->kind == CrashAnnotationKind::Plugin &&
                 annotation.kind != CrashAnnotationKind::Plugin)) {
                best = &annotation;
            }
        }
    }
    return best;
}

} // namespace

const CrashAnnotation *find_owner(const CrashAnnotationBatch *facts, const PatchRecord *owner) noexcept {
    // Resolve pointer identity through copied facts without accessing the patch's potentially damaged state.
    for (auto *batch = facts; batch; batch = batch->next) {
        for (const auto &annotation : batch->records) {
            if (annotation.kind == CrashAnnotationKind::Patch && annotation.owner == owner) {
                return &annotation;
            }
        }
    }
    return nullptr;
}

void append_address(ReportWriter &report, const CrashAnnotationBatch *facts, std::uintptr_t address) noexcept {
    report.hex(address);
    // Prefer copied module identity; ask Windows only when no published image contains the address.
    if (const auto *image = find_module_annotation(facts, address)) {
        report.append(" (");
        report.append_structured(image->module);
        report.append("+");
        report.hex(address - image->address);
        report.append(", timestamp=");
        report.hex(image->timestamp);
        report.append(")");
    } else {
        CrashModuleRecord module{};
        if (resolve_module(address, module)) {
            report.append(" (");
            report.append_structured(module.basename.data());
            report.append("+");
            report.hex(address - module.base);
            report.append(")");
        }
    }

    // Operation records can refer to a separately published patch identity for their owner label.
    if (const auto *annotation = find_annotation(facts, address)) {
        const auto *owner = annotation->owner ? find_owner(facts, annotation->owner) : nullptr;
        report.append(" [");
        report.append(annotation_kind_name(annotation->kind));
        report.append(" owner=");
        report.append_structured(owner ? owner->label : annotation->label);
        if (annotation->kind != CrashAnnotationKind::Plugin && !annotation->detail.empty()) {
            report.append(": ");
            report.append_structured(annotation->detail);
        }
        report.append("]");
    }
}

namespace {

// Appends inspected bytes in rows of at most 16, labeled with their original virtual addresses.
void append_hex_dump(ReportWriter &report, std::span<const std::byte> bytes, std::uintptr_t base) noexcept {
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

} // namespace

void append_stack_frames(ReportWriter &report, [[maybe_unused]] CrashWorkspace &workspace, const CrashAnnotationBatch *facts,
                         const CONTEXT &context) noexcept {
    report.append("\r\nStack frames:\r\n");
#if defined(_M_X64)
    // x64 unwind metadata advances a private context copy and stops if malformed state makes no progress.
    workspace.unwind_context = context;
    std::size_t count{};
    while (count < kMaximumStackFrames && workspace.unwind_context.Rip != 0) {
        report.append("  #");
        report.decimal(count);
        report.append(" ");
        append_address(report, facts, workspace.unwind_context.Rip);
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
    // Follow saved EBP values while each guarded read leads to a higher stack address.
    report.append("  #0 ");
    append_address(report, facts, context.Eip);
    report.append("\r\n");

    struct Frame {
        std::uint32_t next;
        std::uint32_t return_address;
    };

    auto frame_pointer = static_cast<std::uintptr_t>(context.Ebp);
    for (std::size_t index = 1; index < kMaximumStackFrames; ++index) {
        Frame frame{};
        if (!guarded_copy(&frame, reinterpret_cast<const void *>(frame_pointer), sizeof(frame)) || frame.next <= frame_pointer ||
            frame.return_address == 0) {
            break;
        }

        report.append("  #");
        report.decimal(index);
        report.append(" ");
        append_address(report, facts, frame.return_address);
        report.append("\r\n");
        frame_pointer = frame.next;
    }
#endif
}

void append_memory_windows(ReportWriter &report, CrashWorkspace &workspace, const CrashAnnotationBatch *facts,
                           std::uintptr_t instruction_pointer, std::uintptr_t stack_pointer) noexcept {
    // Capture the readable instruction prefix independently so an invalid stack does not hide fault-site bytes.
    const auto instruction_size = readable_extent(instruction_pointer, workspace.instruction_window.size());
    report.append("\r\nInstruction bytes:\r\n");
    if (instruction_size == 0 ||
        !guarded_copy(workspace.instruction_window.data(), reinterpret_cast<const void *>(instruction_pointer), instruction_size)) {
        report.append("  (unavailable)\r\n");
    } else {
        append_hex_dump(report, std::span{workspace.instruction_window}.first(instruction_size), instruction_pointer);
    }

    // One guarded stack snapshot supplies both the displayed prefix and the larger scan for possible return addresses.
    const auto scan_size = readable_extent(stack_pointer, workspace.stack_scan.size());
    const auto stack_copied =
        scan_size != 0 && guarded_copy(workspace.stack_scan.data(), reinterpret_cast<const void *>(stack_pointer), scan_size);
    report.append("\r\nStack window:\r\n");
    if (!stack_copied) {
        report.append("  (unavailable)\r\n");
    } else {
        append_hex_dump(report, std::span{workspace.stack_scan}.first(std::min(scan_size, kStackWindowSize)), stack_pointer);
    }

    // Treat pointer-sized stack values as candidates when they resolve to a module or recorded address range.
    report.append("\r\nAnnotated stack candidates:\r\n");
    std::size_t candidates{};
    if (stack_copied) {
        for (std::size_t offset = 0; offset + sizeof(std::uintptr_t) <= scan_size && candidates < kMaximumStackCandidates;
             offset += sizeof(std::uintptr_t)) {
            std::uintptr_t value{};
            std::memcpy(&value, workspace.stack_scan.data() + offset, sizeof(value));
            CrashModuleRecord module{};
            if (find_annotation(facts, value) == nullptr && !resolve_module(value, module)) {
                continue;
            }

            report.append("  [SP+");
            report.hex(offset);
            report.append("] ");
            append_address(report, facts, value);
            report.append("\r\n");
            ++candidates;
        }
    }

    if (candidates == 0) {
        report.append("  (none)\r\n");
    }
}

void append_inventory(ReportWriter &report, const CrashAnnotationBatch *facts) noexcept {
    report.append("\r\nImages, plugins and patch owners:\r\n");
    // Keep operation ranges in address annotations; the inventory lists only image and owner identities.
    for (auto *batch = facts; batch && !report.full(); batch = batch->next) {
        for (const auto &annotation : batch->records) {
            if (report.full()) {
                return;
            }
            if (annotation.kind != CrashAnnotationKind::Module && annotation.kind != CrashAnnotationKind::Plugin &&
                annotation.kind != CrashAnnotationKind::Patch) {
                continue;
            }

            report.append("  ");
            report.append(annotation_kind_name(annotation.kind));
            report.append(" ");
            report.append_structured(annotation.label);
            if (!annotation.detail.empty()) {
                report.append(" ");
                report.append_structured(annotation.detail);
            }
            if (!annotation.module.empty()) {
                report.append(" module=");
                report.append_structured(annotation.module);
            }
            if (annotation.address) {
                report.append(" base=");
                report.hex(annotation.address);
                report.append(" size=");
                report.hex(annotation.size);
                report.append(" timestamp=");
                report.hex(annotation.timestamp);
            }
            report.append("\r\n");
        }
    }
}

} // namespace crash_detail

void CrashReporter::append_session(ReportWriter &report) const noexcept {
    // Begin with version, process and session identity so even a truncated report remains attributable.
    SYSTEMTIME utc{};
    GetSystemTime(&utc);
    report.append("UTC: ");
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
    report.append("Z\r\nPID: ");
    report.decimal(GetCurrentProcessId());
    report.append("\r\nThread: ");
    report.decimal(GetCurrentThreadId());
    report.append("\r\nFusion Cutter: ");
    report.append(FC_VERSION_STRING);
    report.append("\r\nExecutable: ");
    report.append_structured(session_.executable_basename);
    report.append("\r\nHost role: ");
    report.append(session_.role == FC_ROLE_SERVER ? "Server" : "Client");
    report.append("\r\n");

    // Acquire pairs with set_target's publication so the profile text is complete before a capture reads it.
    report.append("Target: architecture=");
#if defined(_M_X64)
    report.append("x64");
#else
    report.append("x86");
#endif
    report.append(", profile=");
    if (target_ready_.load(std::memory_order_acquire)) {
        report.append_structured(target_profile_);
    } else {
        report.append("Unknown");
    }
    report.append("\r\n");
}

} // namespace fc::runtime
