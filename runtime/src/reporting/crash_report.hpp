#pragma once

#include "crash_reporter.hpp"
#include "bounded_text.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <vector>

namespace fc::runtime {

// Holds the module identity resolved from an address while composing a crash report.
struct CrashModuleRecord {
    std::uintptr_t base{};
    std::size_t size{};
    std::array<char, MAX_PATH> basename{};
};

// Preallocated buffers hold report text and inspected memory while one capture owns the reporter's handling flag.
struct CrashWorkspace {
    std::array<char, 64U * 1024U> report{};
    CONTEXT unwind_context{};
    std::array<std::byte, 0x800> stack_scan{};
    std::array<std::byte, 32> instruction_window{};
};

// Links immutable copies of facts published by the control thread and retained until reporter destruction.
struct CrashAnnotationBatch {
    CrashAnnotationBatch *next{};
    std::vector<CrashAnnotation> records;
};

// Writes bounded text and numbers without allocating; size() delimits output, which has no added terminator.
class ReportWriter final {
  public:
    // Borrows writable output storage that must remain valid throughout the writer's use.
    explicit ReportWriter(std::span<char> output) noexcept : output_(output) {}

    // Appends text with its line breaks; truncation preserves valid UTF-8 boundaries and marks the end.
    void append(std::string_view value) noexcept {
        append_bytes(value, false);
    }

    // Appends a field with ASCII controls replaced by spaces, preserving valid UTF-8 boundaries when truncated.
    void append_structured(std::string_view value) noexcept {
        append_bytes(value, true);
    }

    // Appends a decimal number when its conversion fits the fixed local buffer.
    template <class Value> void decimal(Value value) noexcept {
        std::array<char, 32> buffer{};
        const auto converted = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
        if (converted.ec == std::errc{}) {
            append({buffer.data(), converted.ptr});
        }
    }

    // Appends an integer in hexadecimal with a 0x prefix.
    template <class Value> void hex(Value value) noexcept {
        append("0x");
        std::array<char, 2 * sizeof(Value)> buffer{};
        const auto converted = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value, 16);
        if (converted.ec == std::errc{}) {
            append({buffer.data(), converted.ptr});
        }
    }

    bool full() const noexcept {
        return truncated_;
    }

    std::size_t size() const noexcept {
        return size_;
    }

  private:
    // Copies a field in the requested form; overflow shortens prior text to leave room for the marker.
    void append_bytes(std::string_view value, bool structured) noexcept {
        constexpr std::string_view marker = "\r\n[Crash report truncated]\r\n";
        if (truncated_ || value.empty()) {
            return;
        }

        const auto fits = value.size() <= output_.size() - size_;
        const auto marker_size = std::min(marker.size(), output_.size());
        const auto content_limit = output_.size() - marker_size;
        if (!fits && size_ > content_limit) {
            size_ = reporting::utf8_prefix({output_.data(), size_}, content_limit);
        }

        const auto available = fits ? value.size() : content_limit - size_;
        std::size_t copied{};
        if (structured) {
            copied = reporting::copy_structured_bytes(output_.subspan(size_, available), value);
        } else {
            copied = reporting::utf8_prefix(value, available);
            if (copied != 0) {
                std::memcpy(output_.data() + size_, value.data(), copied);
            }
        }
        size_ += copied;

        if (!fits) {
            std::memcpy(output_.data() + size_, marker.data(), marker_size);
            size_ += marker_size;
            truncated_ = true;
        }
    }

    std::span<char> output_;
    std::size_t size_{};
    bool truncated_{};
};

namespace crash_detail {

// Suppresses capture while this thread performs the reporter's own guarded reads or unwinding.
extern thread_local bool reporter_read_active;
inline constexpr std::uint32_t kMaximumCrashAttempts = 4;
inline constexpr std::uint32_t kStatusFatalAppExit = 0x40000015U;
inline constexpr std::size_t kMaximumStackFrames = 32;
inline constexpr std::size_t kMaximumStackCandidates = 40;
inline constexpr std::size_t kStackWindowSize = 256;

// Writes UTF-8 debugger/console text of at most 1023 bytes using fixed storage, independently of normal logging.
void write_fallback(std::string_view text) noexcept;
// Returns up to maximum readable bytes from address according to current mappings; it does not retain those mappings.
std::size_t readable_extent(std::uintptr_t address, std::size_t maximum) noexcept;
// Copies size bytes under SEH after checking source mappings; false may leave destination partly written.
// The caller supplies destination storage for size bytes; a zero-byte copy succeeds without accessing either pointer.
bool guarded_copy(void *destination, const void *source, std::size_t size) noexcept;
#if defined(_M_X64)
// Advances an x64 context by one stack frame with reporter capture suppressed; false may leave it partly updated.
bool unwind_once(CONTEXT &) noexcept;
#endif
// Resolves a currently loaded module's bounds and basename; false leaves output unspecified and no module reference is retained.
bool resolve_module(std::uintptr_t address, CrashModuleRecord &) noexcept;
// Finds a copied patch identity by pointer equality; the returned record lives as long as the retained batches.
const CrashAnnotation *find_owner(const CrashAnnotationBatch *, const PatchRecord *) noexcept;
// Appends an address with its module offset and the most specific recorded owner or operation.
void append_address(ReportWriter &, const CrashAnnotationBatch *, std::uintptr_t address) noexcept;
// Appends up to kMaximumStackFrames using x64 unwind metadata or an increasing x86 frame-pointer chain.
void append_stack_frames(ReportWriter &, CrashWorkspace &, const CrashAnnotationBatch *, const CONTEXT &) noexcept;
// Appends readable instruction/stack bytes and bounded possible return addresses, without claiming they are frames.
void append_memory_windows(ReportWriter &, CrashWorkspace &, const CrashAnnotationBatch *, std::uintptr_t instruction_pointer,
                           std::uintptr_t stack_pointer) noexcept;
// Appends copied module, plugin and patch identities until the bounded writer truncates.
void append_inventory(ReportWriter &, const CrashAnnotationBatch *) noexcept;

} // namespace crash_detail

} // namespace fc::runtime
