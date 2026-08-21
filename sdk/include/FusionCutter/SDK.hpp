#pragma once

#include <FusionCutter/PluginApi.h>

#include <winsock2.h>
#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <array>
#include <cassert>
#include <bit>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <format>
#include <functional>
#include <initializer_list>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace fc {

// Strongly typed mirrors of the stable C ABI vocabulary used in plugin definitions and callback contexts.
enum class TargetLayout : std::uint32_t {
    GameSpyRetail = FC_LAYOUT_GAMESPY_RETAIL,
    SteamRetail = FC_LAYOUT_STEAM_RETAIL,
    GOGRetail = FC_LAYOUT_GOG_RETAIL,
    ModTools = FC_LAYOUT_MOD_TOOLS,
    ClassicCollection = FC_LAYOUT_CLASSIC_COLLECTION,
};

enum class HostRole : std::uint32_t {
    Client = FC_HOST_ROLE_CLIENT,
    Server = FC_HOST_ROLE_SERVER,
    All = FC_HOST_ROLE_ALL,
};

enum class TargetImage : std::uint32_t {
    Game = FC_IMAGE_GAME,
    Bootstrap = FC_IMAGE_BOOTSTRAP,
    GalaxyPeer = FC_IMAGE_GALAXY_PEER,
};

enum class Architecture : std::uint32_t {
    X86 = FC_ARCH_X86,
    X64 = FC_ARCH_X64,
};

enum class FailurePolicy : std::uint32_t {
    Continue = FC_FAILURE_CONTINUE,
    Fatal = FC_FAILURE_FATAL,
};

enum class LogLevel : std::uint32_t {
    Off = FC_LOG_OFF,
    Error = FC_LOG_ERROR,
    Warning = FC_LOG_WARNING,
    Info = FC_LOG_INFO,
    Debug = FC_LOG_DEBUG,
};

// Byte offset from the selected image base; arithmetic is validated when a location enters a patch plan.
struct Rva {
    std::uint32_t value{};
};

// Callback-scoped target facts; image_profile is borrowed from framework-owned process-lifetime catalog storage.
struct TargetInfo {
    TargetLayout layout{};
    HostRole role{};
    Architecture architecture{};
    std::string_view image_profile;
};

// Expected author-facing failure with a useful cause and optional operation attribution.
struct Error {
    std::string message;
    std::string operation;
};

using Result = std::expected<void, Error>;

// Native data and function concepts define the values that may cross generated native call boundaries.
template <class T>
concept NativeData = std::is_trivially_copyable_v<T>;

template <class T>
concept NativeFunction = std::is_pointer_v<T> && std::is_function_v<std::remove_pointer_t<T>>;

namespace detail {

[[nodiscard]] inline FC_StringView string_view(std::string_view value) noexcept {
    return {.data = value.data(), .size = static_cast<std::uint32_t>(value.size())};
}

[[nodiscard]] inline FC_ByteView byte_view(std::span<const std::byte> value) noexcept {
    return {.data = reinterpret_cast<const std::uint8_t*>(value.data()),
            .size = static_cast<std::uint32_t>(value.size())};
}

[[nodiscard]] inline std::string_view string_view(FC_StringView value) noexcept {
    return {value.data, value.size};
}

[[nodiscard]] inline FC_StringView optional_string_view(const std::optional<std::string>& value) noexcept {
    return value ? string_view(*value) : FC_StringView{};
}

inline void set_error(const FC_ErrorSink* sink, std::string_view message, std::string_view operation = {}) noexcept {
    if (sink != nullptr && sink->set != nullptr) {
        sink->set(sink->context, string_view(message), string_view(operation));
    }
}

[[nodiscard]] constexpr unsigned char fold_ascii(unsigned char value) noexcept {
    return value >= 'A' && value <= 'Z' ? static_cast<unsigned char>(value + ('a' - 'A')) : value;
}

[[nodiscard]] constexpr bool equal_ascii_case_insensitive(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (fold_ascii(static_cast<unsigned char>(left[index])) !=
            fold_ascii(static_cast<unsigned char>(right[index]))) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] constexpr bool valid_id(std::string_view value) noexcept {
    if (value.empty() || value.size() > 64) {
        return false;
    }
    const auto is_letter = [](unsigned char character) {
        return (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z');
    };
    const auto is_digit = [](unsigned char character) {
        return character >= '0' && character <= '9';
    };
    if (!is_letter(static_cast<unsigned char>(value.front()))) {
        return false;
    }
    return std::ranges::all_of(value, [&](char character) {
        const auto byte = static_cast<unsigned char>(character);
        return is_letter(byte) || is_digit(byte) || byte == '_';
    });
}

// FusionCutter identifies framework-owned configuration and diagnostics, so it cannot also identify a plugin.
[[nodiscard]] constexpr bool reserved_plugin_id(std::string_view value) noexcept {
    return equal_ascii_case_insensitive(value, "FusionCutter");
}

class ContextFactory;
struct InterfaceQueryAdapter;
template <auto Method, class Object, class Signature = decltype(Method)> struct InterfaceFunctionBridge;
template <auto Method, class Object, class Signature = decltype(Method)> struct ObservationBridge;
struct PatchConcept;
struct PluginAccess;
struct PlanAccess;
struct StringMapAccess;
struct SupportConcept;

template <class Settings, class Value> class ValueSettingBuilder;

template <class Settings, class Enum> class ChoiceSettingBuilder;

template <class Settings> struct SettingEntryAccess;

template <class PlanFunction> struct PlanCallableTraits;

template <class Handler> class HandlerAdapter;

template <class Handler> class PatchModel;

template <class Method> struct InterfaceMethodTraits;

// Nonthrowing member methods are lowered to the one context/thunk pair allowed in a public interface value.
template <class Result, class Class, class... Arguments>
struct InterfaceMethodTraits<Result (Class::*)(Arguments...) noexcept> {
    using signature = Result(Arguments...) noexcept;
};

template <class Result, class Class, class... Arguments>
struct InterfaceMethodTraits<Result (Class::*)(Arguments...) const noexcept> {
    using signature = Result(Arguments...) noexcept;
};

} // namespace detail

// A copyable logger remains bound to one patch report token and may be retained for work after installation.
class Logger {
  public:
    [[nodiscard]] bool enabled(LogLevel level) const noexcept {
        return level != LogLevel::Off && host_ != nullptr && host_->log_enabled != nullptr &&
               host_->log_enabled(host_->context, report_, static_cast<FC_LogLevel>(level)) == FC_TRUE;
    }

    void write(LogLevel level, std::string_view message) const noexcept {
        if (level == LogLevel::Off || host_ == nullptr || host_->log_write == nullptr) {
            return;
        }
        host_->log_write(host_->context, report_, static_cast<FC_LogLevel>(level), detail::string_view(message));
    }

    template <class... Args> void error(std::format_string<Args...> format, Args&&... args) const noexcept {
        write_formatted(LogLevel::Error, format, std::forward<Args>(args)...);
    }

    template <class... Args> void warning(std::format_string<Args...> format, Args&&... args) const noexcept {
        write_formatted(LogLevel::Warning, format, std::forward<Args>(args)...);
    }

    template <class... Args> void info(std::format_string<Args...> format, Args&&... args) const noexcept {
        write_formatted(LogLevel::Info, format, std::forward<Args>(args)...);
    }

    template <class... Args> void debug(std::format_string<Args...> format, Args&&... args) const noexcept {
        write_formatted(LogLevel::Debug, format, std::forward<Args>(args)...);
    }

  private:
    Logger(const FC_HostApi* host, FC_ReportToken report) noexcept : host_(host), report_(report) {}

    template <class... Args>
    void write_formatted(LogLevel level, std::format_string<Args...> format, Args&&... args) const noexcept {
        if (!enabled(level)) {
            return;
        }
        try {
            write(level, std::format(format, std::forward<Args>(args)...));
        } catch (...) {
            write(level, "Fusion Cutter SDK could not format this log message");
        }
    }

    const FC_HostApi* host_ = nullptr;
    FC_ReportToken report_ = nullptr;

    friend class detail::ContextFactory;
};

// A callback-scoped bounded writer for one patch's live status section.
class StatusWriter {
  public:
    bool add(std::string_view label, std::string_view value) noexcept {
        return sink_ != nullptr && sink_->add_text != nullptr &&
               sink_->add_text(sink_->context, detail::string_view(label), detail::string_view(value)) == FC_TRUE;
    }

    template <std::integral T>
        requires(!std::same_as<std::remove_cv_t<T>, bool>)
    bool add(std::string_view label, T value) noexcept {
        if constexpr (std::is_signed_v<T>) {
            return sink_ != nullptr && sink_->add_signed != nullptr &&
                   sink_->add_signed(sink_->context, detail::string_view(label), static_cast<std::int64_t>(value)) ==
                       FC_TRUE;
        } else {
            return sink_ != nullptr && sink_->add_unsigned != nullptr &&
                   sink_->add_unsigned(sink_->context, detail::string_view(label), static_cast<std::uint64_t>(value)) ==
                       FC_TRUE;
        }
    }

    bool add(std::string_view label, double value) noexcept {
        return sink_ != nullptr && sink_->add_floating != nullptr &&
               sink_->add_floating(sink_->context, detail::string_view(label), value) == FC_TRUE;
    }

    bool add(std::string_view label, bool value) noexcept {
        return sink_ != nullptr && sink_->add_boolean != nullptr &&
               sink_->add_boolean(sink_->context, detail::string_view(label), value ? FC_TRUE : FC_FALSE) == FC_TRUE;
    }

  private:
    explicit StatusWriter(const FC_StatusSink* sink) noexcept : sink_(sink) {}

    const FC_StatusSink* sink_ = nullptr;

    friend class detail::ContextFactory;
};

// Prepare channel request; capacity counts pending records and max_record_size counts payload bytes.
struct TraceDefinition {
    std::string_view name;
    std::uint32_t capacity{};
    std::uint32_t max_record_size{};
    std::uint32_t version = 1;
};

// Monotonic snapshot for one channel; dropped counts valid records lost after submission pressure.
struct TraceHealth {
    std::uint64_t accepted{};
    std::uint64_t written{};
    std::uint64_t dropped{};
    bool file_limit_reached{};
    bool output_failed{};
};

// Move-only reference to a framework-owned trace channel. try_write is bounded, nonblocking, and thread-safe.
class TraceChannel {
  public:
    TraceChannel() noexcept = default;
    TraceChannel(const TraceChannel&) = delete;
    TraceChannel& operator=(const TraceChannel&) = delete;
    TraceChannel(TraceChannel&& other) noexcept
        : host_(std::exchange(other.host_, nullptr)), handle_(std::exchange(other.handle_, nullptr)) {}
    TraceChannel& operator=(TraceChannel&& other) noexcept {
        if (this != &other) {
            host_ = std::exchange(other.host_, nullptr);
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }

    [[nodiscard]] bool enabled() const noexcept {
        return host_ != nullptr && handle_ != nullptr && host_->trace_enabled != nullptr &&
               host_->trace_enabled(host_->context, handle_) == FC_TRUE;
    }

    template <class Record>
        requires requires { sizeof(Record); } && std::is_standard_layout_v<Record> &&
                 std::is_trivially_copyable_v<Record>
    [[nodiscard]] bool try_write(const Record& record) noexcept {
        const auto bytes = std::as_bytes(std::span{&record, std::size_t{1}});
        return try_write(bytes);
    }

    [[nodiscard]] bool try_write(std::span<const std::byte> record) noexcept {
        return host_ != nullptr && handle_ != nullptr && host_->trace_try_write != nullptr &&
               host_->trace_try_write(host_->context, handle_, detail::byte_view(record)) == FC_TRUE;
    }

    [[nodiscard]] TraceHealth health() const noexcept {
        FC_TraceHealth native{.struct_size = sizeof(FC_TraceHealth)};
        if (host_ != nullptr && handle_ != nullptr && host_->trace_health != nullptr) {
            host_->trace_health(host_->context, handle_, &native);
        }
        return {.accepted = native.accepted,
                .written = native.written,
                .dropped = native.dropped,
                .file_limit_reached = native.file_limit_reached == FC_TRUE,
                .output_failed = native.output_failed == FC_TRUE};
    }

  private:
    TraceChannel(const FC_HostApi* host, FC_TraceHandle handle) noexcept : host_(host), handle_(handle) {}

    const FC_HostApi* host_ = nullptr;
    FC_TraceHandle handle_ = nullptr;

    friend class detail::ContextFactory;
};

// Describes the bytes a location is expected to contain; every location and Plan callback submission owns its copy.
class Evidence {
  public:
    Evidence() = default;

  private:
    FC_EvidenceKind kind_ = FC_EVIDENCE_NONE;
    std::vector<std::byte> bytes_;
    std::vector<std::byte> mask_;
    std::uint32_t target_rva_{};

    friend struct detail::PlanAccess;
    friend Evidence exact_bytes(std::span<const std::byte> bytes);
    friend Evidence masked_bytes(std::span<const std::byte> bytes, std::span<const std::byte> mask);
    template <NativeData T> friend Evidence expect(const T& expected_value);
    template <class Location> friend Evidence points_to(const Location& expected_location);
    template <class Location> friend Evidence direct_call_to(const Location& expected_location);
    template <class Location> friend Evidence direct_jump_to(const Location& expected_location);
};

// Evidence factories retain their expected state by value so temporary author data is safe to compose.
[[nodiscard]] inline Evidence exact_bytes(std::span<const std::byte> bytes) {
    Evidence result;
    result.kind_ = FC_EVIDENCE_EXACT_BYTES;
    result.bytes_.assign(bytes.begin(), bytes.end());
    return result;
}

template <std::size_t Size> [[nodiscard]] Evidence exact_bytes(const std::array<std::byte, Size>& bytes) {
    return exact_bytes(std::span{bytes});
}

[[nodiscard]] inline Evidence exact_bytes(std::initializer_list<std::byte> bytes) {
    return exact_bytes(std::span{bytes.begin(), bytes.size()});
}

[[nodiscard]] inline Evidence masked_bytes(std::span<const std::byte> bytes, std::span<const std::byte> mask) {
    Evidence result;
    result.kind_ = FC_EVIDENCE_MASKED_BYTES;
    result.bytes_.assign(bytes.begin(), bytes.end());
    result.mask_.assign(mask.begin(), mask.end());
    return result;
}

template <std::size_t Size>
[[nodiscard]] Evidence masked_bytes(const std::array<std::byte, Size>& bytes, const std::array<std::byte, Size>& mask) {
    return masked_bytes(std::span{bytes}, std::span{mask});
}

[[nodiscard]] inline Evidence masked_bytes(std::initializer_list<std::byte> bytes,
                                           std::initializer_list<std::byte> mask) {
    return masked_bytes(std::span{bytes.begin(), bytes.size()}, std::span{mask.begin(), mask.size()});
}

template <NativeData T> [[nodiscard]] Evidence expect(const T& expected_value) {
    return exact_bytes(std::as_bytes(std::span{&expected_value, std::size_t{1}}));
}

template <class Location> [[nodiscard]] Evidence points_to(const Location& expected_location) {
    Evidence result;
    result.kind_ = FC_EVIDENCE_POINTS_TO;
    result.target_rva_ = expected_location.rva.value;
    return result;
}

template <class Location> [[nodiscard]] Evidence direct_call_to(const Location& expected_location) {
    Evidence result;
    result.kind_ = FC_EVIDENCE_DIRECT_CALL_TO;
    result.target_rva_ = expected_location.rva.value;
    return result;
}

template <class Location> [[nodiscard]] Evidence direct_jump_to(const Location& expected_location) {
    Evidence result;
    result.kind_ = FC_EVIDENCE_DIRECT_JUMP_TO;
    result.target_rva_ = expected_location.rva.value;
    return result;
}

// Separates the logical function signature from a reviewed physical register/stack calling layout.
template <class Signature, class Layout> struct NativeCall {
    using signature = Signature;
    using layout = Layout;
};

namespace abi {

// Compile-time storage tags describe every argument, result, cleanup rule, and architecture explicitly.
template <class... Homes> struct args {};

template <class Home> struct result {};

template <class Home> struct hidden_result {};

template <std::size_t Offset> struct stack {
    static constexpr std::size_t offset = Offset;
};

struct caller_cleanup {};
struct callee_cleanup {};
struct no_cleanup {};

#define FC_SDK_REGISTER_TAG(name)                                                                                      \
    struct name {}
FC_SDK_REGISTER_TAG(eax);
FC_SDK_REGISTER_TAG(ebx);
FC_SDK_REGISTER_TAG(ecx);
FC_SDK_REGISTER_TAG(edx);
FC_SDK_REGISTER_TAG(esi);
FC_SDK_REGISTER_TAG(edi);
FC_SDK_REGISTER_TAG(ebp);
FC_SDK_REGISTER_TAG(st0);
FC_SDK_REGISTER_TAG(rax);
FC_SDK_REGISTER_TAG(rbx);
FC_SDK_REGISTER_TAG(rcx);
FC_SDK_REGISTER_TAG(rdx);
FC_SDK_REGISTER_TAG(rsi);
FC_SDK_REGISTER_TAG(rdi);
FC_SDK_REGISTER_TAG(rbp);
FC_SDK_REGISTER_TAG(r8);
FC_SDK_REGISTER_TAG(r9);
FC_SDK_REGISTER_TAG(r10);
FC_SDK_REGISTER_TAG(r11);
FC_SDK_REGISTER_TAG(r12);
FC_SDK_REGISTER_TAG(r13);
FC_SDK_REGISTER_TAG(r14);
FC_SDK_REGISTER_TAG(r15);
FC_SDK_REGISTER_TAG(xmm0);
FC_SDK_REGISTER_TAG(xmm1);
FC_SDK_REGISTER_TAG(xmm2);
FC_SDK_REGISTER_TAG(xmm3);
FC_SDK_REGISTER_TAG(xmm4);
FC_SDK_REGISTER_TAG(xmm5);
FC_SDK_REGISTER_TAG(xmm6);
FC_SDK_REGISTER_TAG(xmm7);
FC_SDK_REGISTER_TAG(xmm8);
FC_SDK_REGISTER_TAG(xmm9);
FC_SDK_REGISTER_TAG(xmm10);
FC_SDK_REGISTER_TAG(xmm11);
FC_SDK_REGISTER_TAG(xmm12);
FC_SDK_REGISTER_TAG(xmm13);
FC_SDK_REGISTER_TAG(xmm14);
FC_SDK_REGISTER_TAG(xmm15);
#undef FC_SDK_REGISTER_TAG

template <class Arguments, class Return, class Cleanup> struct x86 {};

template <class Arguments, class Return, class Cleanup = no_cleanup> struct x64 {};

} // namespace abi

namespace detail {

template <class T> struct IsExplicitNativeCall : std::false_type {};

template <class Signature, class Layout> struct IsExplicitNativeCall<NativeCall<Signature, Layout>> : std::true_type {};

template <class T> inline constexpr bool is_explicit_native_call_v = IsExplicitNativeCall<T>::value;

template <class T>
concept NativeCallType = NativeFunction<T> || is_explicit_native_call_v<T>;

template <class T>
concept ResolvableNativeFunction = NativeFunction<T> || is_explicit_native_call_v<T>;

} // namespace detail

// Named image locations retain optional evidence by value; every rva is relative to the selected support image.
template <class T, std::size_t Count = 1>
    requires NativeData<T> && (Count > 0)
struct DataLocation {
    using value_type = T;
    static constexpr std::size_t count = Count;

    Rva rva;
    std::string_view name{};
    std::string_view label{};
    Evidence evidence{};
};

template <class Call>
    requires detail::ResolvableNativeFunction<Call>
struct FunctionLocation {
    using call_type = Call;

    Rva rva;
    std::string_view name{};
    std::string_view label{};
    Evidence evidence{};
};

template <class Call>
    requires detail::NativeCallType<Call>
struct CallLocation {
    using call_type = Call;

    Rva rva;
    std::string_view name{};
    std::string_view label{};
    Evidence evidence{};
};

struct CodeLocation {
    Rva rva;
    std::string_view name{};
    std::string_view label{};
    Evidence evidence{};
};

struct VtableLocation {
    Rva rva;
    std::string_view name{};
    std::string_view label{};
    Evidence evidence{};
};

template <class Function>
    requires NativeFunction<Function>
using VtableSlotLocation = DataLocation<Function>;

struct LocationMetadata {
    std::string_view name{};
    std::string_view label{};
    Evidence evidence{};
};

// Invalid derivations carry the maximum RVA sentinel so validation of the patch plan reports the failure.
template <class Function>
[[nodiscard]] VtableSlotLocation<Function> vtable_slot(const VtableLocation& table, std::size_t index,
                                                       LocationMetadata metadata = {}) {
    const auto byte_offset = index * sizeof(Function);
    if (index > std::numeric_limits<std::uint32_t>::max() / sizeof(Function) ||
        byte_offset > std::numeric_limits<std::uint32_t>::max() - table.rva.value) {
        return {.rva = {std::numeric_limits<std::uint32_t>::max()},
                .name = metadata.name,
                .label = metadata.label,
                .evidence = std::move(metadata.evidence)};
    }
    return {.rva = {table.rva.value + static_cast<std::uint32_t>(byte_offset)},
            .name = metadata.name,
            .label = metadata.label,
            .evidence = std::move(metadata.evidence)};
}

template <class T, std::size_t Count>
[[nodiscard]] DataLocation<T> element(const DataLocation<T, Count>& array, std::size_t index,
                                      LocationMetadata metadata = {}) {
    // Preserve invalid arithmetic as a value so the Plan callback reports it through the normal failure path.
    const auto byte_offset = index * sizeof(T);
    if (index >= Count || byte_offset > std::numeric_limits<std::uint32_t>::max() - array.rva.value) {
        return {.rva = {std::numeric_limits<std::uint32_t>::max()},
                .name = metadata.name,
                .label = metadata.label,
                .evidence = std::move(metadata.evidence)};
    }
    return {.rva = {array.rva.value + static_cast<std::uint32_t>(byte_offset)},
            .name = metadata.name,
            .label = metadata.label,
            .evidence = std::move(metadata.evidence)};
}

using SimdRegister = FC_SimdRegister;
using CpuContext = FC_CpuContext;

// Borrowed callback surface; all operations and symbolic addresses become invalid when the Plan callback returns.
class Plan;

// Opaque symbolic address into one native allocation owned by the patch plan.
class DataAddress {
  public:
    DataAddress(const DataAddress&) = default;
    DataAddress& operator=(const DataAddress&) = default;

  private:
    DataAddress(Plan* owner, FC_DataHandle handle, std::uint64_t offset, bool one_past) noexcept
        : owner_(owner), handle_(handle), offset_(offset), one_past_(one_past) {}

    Plan* owner_ = nullptr;
    FC_DataHandle handle_ = FC_INVALID_DATA_HANDLE;
    std::uint64_t offset_{};
    bool one_past_{};

    friend class Plan;
    template <NativeData T> friend class DataHandle;
    friend struct detail::PlanAccess;
};

// Only the Prepare phase resolves a symbolic allocation; invalid derivations fail its owning patch plan.
template <NativeData T> class DataHandle {
  public:
    [[nodiscard]] DataAddress base() const noexcept;
    [[nodiscard]] DataAddress element(std::size_t index) const noexcept;
    [[nodiscard]] DataAddress byte_offset(std::size_t offset) const noexcept;
    [[nodiscard]] DataAddress end() const noexcept;

  private:
    DataHandle(Plan* owner, FC_DataHandle handle, std::size_t count) noexcept
        : owner_(owner), handle_(handle), count_(count) {}

    Plan* owner_ = nullptr;
    FC_DataHandle handle_ = FC_INVALID_DATA_HANDLE;
    std::size_t count_{};

    friend class Plan;
    friend class PrepareContext;
};

namespace detail {

template <class Target, FC_WriteKind Kind> struct AddressExpression {
    static constexpr FC_WriteKind kind = Kind;
    Target target;
};

template <class Callback, bool Before> struct ObserverTag {
    static constexpr bool before = Before;
    Callback callback;
};

} // namespace detail

// These expression helpers preserve intent until the patch plan validates and submits the composed request.
template <class Target> [[nodiscard]] auto rel32(Target&& target) {
    return detail::AddressExpression<std::decay_t<Target>, FC_WRITE_REL32>{std::forward<Target>(target)};
}

template <class Target> [[nodiscard]] auto call_to(Target&& target) {
    return detail::AddressExpression<std::decay_t<Target>, FC_WRITE_CALL>{std::forward<Target>(target)};
}

template <class Target> [[nodiscard]] auto jump_to(Target&& target) {
    return detail::AddressExpression<std::decay_t<Target>, FC_WRITE_JUMP>{std::forward<Target>(target)};
}

template <class Callback> [[nodiscard]] auto before(Callback&& callback) {
    return detail::ObserverTag<std::decay_t<Callback>, true>{std::forward<Callback>(callback)};
}

template <class Callback> [[nodiscard]] auto after(Callback&& callback) {
    return detail::ObserverTag<std::decay_t<Callback>, false>{std::forward<Callback>(callback)};
}

// Interface values are bounded copies with a stable layout rather than references to C++ objects across modules.
template <class Interface>
concept InterfaceContract =
    requires {
        { Interface::id } -> std::convertible_to<std::string_view>;
        requires detail::valid_id(std::string_view{Interface::id});
    } && std::is_standard_layout_v<Interface> && std::is_trivially_copyable_v<Interface> &&
    (sizeof(Interface) <= 512) && (alignof(Interface) <= alignof(std::max_align_t));

template <class Signature> class InterfaceFunction;
template <class Signature> class Observation;

template <auto Method, class Object> [[nodiscard]] auto interface_function(Object& object) noexcept;

template <auto Method, class Consumer> [[nodiscard]] auto observation(Consumer& consumer) noexcept;

// Stores a service or policy call as a DLL-safe opaque context and a thunk using the C calling convention.
template <class Result, class... Arguments> class InterfaceFunction<Result(Arguments...) noexcept> {
  public:
    [[nodiscard]] explicit operator bool() const noexcept {
        return callback_ != nullptr;
    }

    Result operator()(Arguments... arguments) const noexcept {
        assert(callback_ != nullptr);
        if constexpr (std::is_void_v<Result>) {
            callback_(context_, std::forward<Arguments>(arguments)...);
        } else {
            return callback_(context_, std::forward<Arguments>(arguments)...);
        }
    }

  private:
    void* context_ = nullptr;
    Result(FC_CALL* callback_)(void* context, Arguments... arguments) noexcept = nullptr;

    template <auto Method, class Object, class Signature> friend struct detail::InterfaceFunctionBridge;
};

// Stores a transparent observation callback whose generated bridge preserves both Win32 ambient error values.
template <class... Arguments> class Observation<void(Arguments...) noexcept> {
  public:
    [[nodiscard]] explicit operator bool() const noexcept {
        return callback_ != nullptr;
    }

    void operator()(Arguments... arguments) const noexcept {
        assert(callback_ != nullptr);
        callback_(context_, std::forward<Arguments>(arguments)...);
    }

  private:
    void* context_ = nullptr;
    void(FC_CALL* callback_)(void* context, Arguments... arguments) noexcept = nullptr;

    template <auto Method, class Consumer, class Signature> friend struct detail::ObservationBridge;
};

namespace detail {

// The helper owns the only write access to InterfaceFunction's physical transport fields.
template <auto Method, class Object, class Result, class Class, class... Arguments>
struct InterfaceFunctionBridge<Method, Object, Result (Class::*)(Arguments...) noexcept> {
    using Function = InterfaceFunction<Result(Arguments...) noexcept>;
    using Value = std::remove_cv_t<Object>;

    [[nodiscard]] static Function make(Object& object) noexcept {
        static_assert(!std::is_const_v<Object> && !std::is_volatile_v<Object>);
        static_assert(std::derived_from<Value, Class> || std::same_as<Value, Class>);
        Function result;
        result.context_ = std::addressof(object);
        result.callback_ = &invoke;
        return result;
    }

  private:
    static Result FC_CALL invoke(void* context, Arguments... arguments) noexcept {
        if constexpr (std::is_void_v<Result>) {
            (static_cast<Value*>(context)->*Method)(std::forward<Arguments>(arguments)...);
        } else {
            return (static_cast<Value*>(context)->*Method)(std::forward<Arguments>(arguments)...);
        }
    }
};

template <auto Method, class Object, class Result, class Class, class... Arguments>
struct InterfaceFunctionBridge<Method, Object, Result (Class::*)(Arguments...) const noexcept> {
    using Function = InterfaceFunction<Result(Arguments...) noexcept>;
    using Value = std::remove_cv_t<Object>;

    [[nodiscard]] static Function make(Object& object) noexcept {
        static_assert(!std::is_volatile_v<Object>);
        static_assert(std::derived_from<Value, Class> || std::same_as<Value, Class>);
        Function result;
        result.context_ = const_cast<Value*>(std::addressof(object));
        result.callback_ = &invoke;
        return result;
    }

  private:
    static Result FC_CALL invoke(void* context, Arguments... arguments) noexcept {
        if constexpr (std::is_void_v<Result>) {
            (static_cast<const Value*>(context)->*Method)(std::forward<Arguments>(arguments)...);
        } else {
            return (static_cast<const Value*>(context)->*Method)(std::forward<Arguments>(arguments)...);
        }
    }
};

// Observation bridges restore the provider's ambient error state after arbitrary consumer work completes.
template <auto Method, class Consumer, class Class, class... Arguments>
struct ObservationBridge<Method, Consumer, void (Class::*)(Arguments...) noexcept> {
    using Callback = Observation<void(Arguments...) noexcept>;
    using Value = std::remove_cv_t<Consumer>;

    [[nodiscard]] static Callback make(Consumer& consumer) noexcept {
        static_assert(!std::is_const_v<Consumer> && !std::is_volatile_v<Consumer>);
        static_assert(std::derived_from<Value, Class> || std::same_as<Value, Class>);
        Callback result;
        result.context_ = std::addressof(consumer);
        result.callback_ = &invoke;
        return result;
    }

  private:
    static void FC_CALL invoke(void* context, Arguments... arguments) noexcept {
        const auto win32_error = GetLastError();
        const auto winsock_error = WSAGetLastError();
        (static_cast<Value*>(context)->*Method)(std::forward<Arguments>(arguments)...);
        WSASetLastError(winsock_error);
        SetLastError(win32_error);
    }
};

template <auto Method, class Consumer, class Class, class... Arguments>
struct ObservationBridge<Method, Consumer, void (Class::*)(Arguments...) const noexcept> {
    using Callback = Observation<void(Arguments...) noexcept>;
    using Value = std::remove_cv_t<Consumer>;

    [[nodiscard]] static Callback make(Consumer& consumer) noexcept {
        static_assert(!std::is_volatile_v<Consumer>);
        static_assert(std::derived_from<Value, Class> || std::same_as<Value, Class>);
        Callback result;
        result.context_ = const_cast<Value*>(std::addressof(consumer));
        result.callback_ = &invoke;
        return result;
    }

  private:
    static void FC_CALL invoke(void* context, Arguments... arguments) noexcept {
        const auto win32_error = GetLastError();
        const auto winsock_error = WSAGetLastError();
        (static_cast<const Value*>(context)->*Method)(std::forward<Arguments>(arguments)...);
        WSASetLastError(winsock_error);
        SetLastError(win32_error);
    }
};

} // namespace detail

// Generates the private C thunk while leaving the service or policy call's error semantics to its contract.
template <auto Method, class Object> [[nodiscard]] auto interface_function(Object& object) noexcept {
    return detail::InterfaceFunctionBridge<Method, Object>::make(object);
}

// Generates the standard transparent callback bridge for observation records retained by the provider.
template <auto Method, class Consumer> [[nodiscard]] auto observation(Consumer& consumer) noexcept {
    return detail::ObservationBridge<Method, Consumer>::make(consumer);
}

// Callback-scoped output that copies the first matching small, DLL-safe interface value into framework storage.
class InterfaceQuery {
  public:
    template <InterfaceContract Interface> void provide(const Interface& value) noexcept {
        if (provided_ || !detail::equal_ascii_case_insensitive(requested_id_, std::string_view{Interface::id}) ||
            output_.size() != sizeof(Interface)) {
            return;
        }
        std::memcpy(output_.data(), &value, sizeof(Interface));
        provided_ = true;
    }

  private:
    std::string_view requested_id_;
    std::span<std::byte> output_;
    bool provided_ = false;

    friend struct detail::InterfaceQueryAdapter;
};

// Completed read-only string settings in schema order; returned views live as long as this map.
class StringMap {
  public:
    struct Entry {
        std::string key;
        std::string value;
    };

    [[nodiscard]] std::optional<std::string_view> find(std::string_view key) const noexcept {
        const auto match = std::ranges::find_if(entries_, [&](const Entry& entry) {
            return detail::equal_ascii_case_insensitive(entry.key, key);
        });
        if (match == entries_.end()) {
            return std::nullopt;
        }
        return match->value;
    }

    [[nodiscard]] std::span<const Entry> entries() const noexcept {
        return entries_;
    }

  private:
    std::vector<Entry> entries_;

    friend struct detail::StringMapAccess;
};

// Declares a key in a finite map of strings; max_length excludes the UTF-8 terminator, and zero is unlimited.
struct StringSetting {
    std::string_view key;
    std::string_view default_value;
    std::string_view description{};
    std::uint32_t max_length{};
    std::string_view environment{};
};

namespace detail {

template <class Value>
inline constexpr bool is_character_integer_v =
    std::same_as<std::remove_cv_t<Value>, char> || std::same_as<std::remove_cv_t<Value>, signed char> ||
    std::same_as<std::remove_cv_t<Value>, unsigned char> || std::same_as<std::remove_cv_t<Value>, wchar_t> ||
    std::same_as<std::remove_cv_t<Value>, char8_t> || std::same_as<std::remove_cv_t<Value>, char16_t> ||
    std::same_as<std::remove_cv_t<Value>, char32_t>;

template <class Value>
concept SettingValue =
    std::same_as<std::remove_cv_t<Value>, bool> || (std::integral<Value> && !is_character_integer_v<Value>) ||
    std::floating_point<Value> || std::same_as<std::remove_cv_t<Value>, std::string>;

template <class Settings> struct SettingEntryData;

} // namespace detail

// Owning normalized declaration normally produced by value(), choice(), or section() composition helpers.
template <class Settings> class SettingEntry {
  public:
    SettingEntry() = default;

  private:
    explicit SettingEntry(std::shared_ptr<const detail::SettingEntryData<Settings>> data) : data_(std::move(data)) {}

    std::shared_ptr<const detail::SettingEntryData<Settings>> data_;

    template <class S, class Value> friend class detail::ValueSettingBuilder;
    template <class S, class Enum> friend class detail::ChoiceSettingBuilder;
    template <class S> friend struct detail::SettingEntryAccess;
};

// A named settings section preserves declaration order for both generated files and resolved value assignment.
template <class Settings> struct SettingsSection {
    std::string id;
    std::vector<SettingEntry<Settings>> entries;
};

// Settings validation runs after each declared value is assigned and may normalize the completed object.
template <class Settings> using SettingsValidator = Result (*)(Settings&);

// One typed schema supports compact settings, sectioned settings, and finite maps of string settings.
template <class Settings> struct SettingsSchema {
    std::vector<SettingEntry<Settings>> entries;
    std::vector<SettingsSection<Settings>> sections;
    SettingsValidator<Settings> validate{};
};

namespace detail {

template <class Settings> struct NoSettingsSchemaTag {};

struct NoSettings {};

template <class Handler, class = void> struct HandlerSettings {
    using type = NoSettings;
};

template <class Handler> struct HandlerSettings<Handler, std::void_t<typename Handler::Settings>> {
    using type = typename Handler::Settings;
};

template <class Handler> using SettingsFor = typename HandlerSettings<Handler>::type;

template <class Settings> struct SettingEntryData {
    std::string section;
    std::string key;
    std::string description;
    std::string environment;
    FC_SettingType type{};
    bool has_range{};
    FC_SettingValue default_value{};
    FC_SettingValue minimum{};
    FC_SettingValue maximum{};
    std::uint32_t max_length{};
    std::string default_string;
    std::vector<std::string> choice_names;
    std::function<void(Settings&, const FC_SettingValue&)> assign;
};

struct StringMapAccess {
    static void assign(StringMap& map, std::string key, std::string value) {
        map.entries_.push_back({.key = std::move(key), .value = std::move(value)});
    }
};

// Maps the exact C++ storage width to the corresponding stable setting kind at compile time.
template <class Value> [[nodiscard]] consteval FC_SettingType setting_type() {
    using Clean = std::remove_cv_t<Value>;
    // Integer branches preserve author storage width instead of normalizing every value to the ABI union width.
    if constexpr (std::same_as<Clean, bool>) {
        return FC_SETTING_BOOLEAN;
    } else if constexpr (std::same_as<Clean, std::string>) {
        return FC_SETTING_STRING;
    } else if constexpr (std::floating_point<Clean>) {
        return sizeof(Clean) == 4 ? FC_SETTING_FLOAT_32 : FC_SETTING_FLOAT_64;
    } else if constexpr (std::is_signed_v<Clean>) {
        if constexpr (sizeof(Clean) == 1) {
            return FC_SETTING_SIGNED_8;
        }
        if constexpr (sizeof(Clean) == 2) {
            return FC_SETTING_SIGNED_16;
        }
        if constexpr (sizeof(Clean) == 4) {
            return FC_SETTING_SIGNED_32;
        }
        return FC_SETTING_SIGNED_64;
    } else {
        if constexpr (sizeof(Clean) == 1) {
            return FC_SETTING_UNSIGNED_8;
        }
        if constexpr (sizeof(Clean) == 2) {
            return FC_SETTING_UNSIGNED_16;
        }
        if constexpr (sizeof(Clean) == 4) {
            return FC_SETTING_UNSIGNED_32;
        }
        return FC_SETTING_UNSIGNED_64;
    }
}

// Places an author value into the active union member selected by setting_type().
template <class Value> [[nodiscard]] FC_SettingValue native_setting_value(const Value& value) {
    FC_SettingValue result{};
    using Clean = std::remove_cv_t<Value>;
    if constexpr (std::same_as<Clean, bool>) {
        result.boolean_value = value ? FC_TRUE : FC_FALSE;
    } else if constexpr (std::same_as<Clean, std::string>) {
        result.string_value = detail::string_view(value);
    } else if constexpr (std::floating_point<Clean>) {
        result.floating_value = static_cast<double>(value);
    } else if constexpr (std::is_signed_v<Clean>) {
        result.signed_value = static_cast<std::int64_t>(value);
    } else {
        result.unsigned_value = static_cast<std::uint64_t>(value);
    }
    return result;
}

// Fluent builders own all backing strings while validating constraints that can be decided during composition.
template <class Settings, class Value> class ValueSettingBuilder {
  public:
    ValueSettingBuilder(std::string key, Value Settings::* member, Value default_value)
        : data_(std::make_shared<SettingEntryData<Settings>>()) {
        // Reject non-finite defaults before they can enter either schema metadata or generated configuration.
        if constexpr (std::floating_point<Value>) {
            if (!std::isfinite(default_value)) {
                throw std::invalid_argument{"A floating setting default must be finite"};
            }
        }
        // String defaults live beside the union so its borrowed view can be repaired during native lowering.
        data_->key = std::move(key);
        data_->type = setting_type<Value>();
        if constexpr (std::same_as<Value, std::string>) {
            data_->default_string = std::move(default_value);
            data_->default_value.string_value = detail::string_view(data_->default_string);
        } else {
            data_->default_value = native_setting_value(default_value);
        }
        // Retain the member binding as the sole path from the flat resolved ABI value to typed handler settings.
        data_->assign = [member](Settings& settings, const FC_SettingValue& value) {
            if constexpr (std::same_as<Value, bool>) {
                settings.*member = value.boolean_value == FC_TRUE;
            } else if constexpr (std::same_as<Value, std::string>) {
                settings.*member = std::string{detail::string_view(value.string_value)};
            } else if constexpr (std::floating_point<Value>) {
                settings.*member = static_cast<Value>(value.floating_value);
            } else if constexpr (std::is_signed_v<Value>) {
                settings.*member = static_cast<Value>(value.signed_value);
            } else {
                settings.*member = static_cast<Value>(value.unsigned_value);
            }
        };
    }

    ValueSettingBuilder description(std::string value) && {
        data_->description = std::move(value);
        return std::move(*this);
    }

    ValueSettingBuilder environment(std::string value) && {
        data_->environment = std::move(value);
        return std::move(*this);
    }

    ValueSettingBuilder range(Value minimum, Value maximum) &&
        requires(std::integral<Value> && !std::same_as<Value, bool>) || std::floating_point<Value>
    {
        if constexpr (std::floating_point<Value>) {
            if (!std::isfinite(minimum) || !std::isfinite(maximum)) {
                throw std::invalid_argument{"Floating setting range bounds must be finite"};
            }
        }
        const auto default_value = [&]() {
            if constexpr (std::floating_point<Value>) {
                return static_cast<Value>(data_->default_value.floating_value);
            } else if constexpr (std::is_signed_v<Value>) {
                return static_cast<Value>(data_->default_value.signed_value);
            } else {
                return static_cast<Value>(data_->default_value.unsigned_value);
            }
        }();
        if (minimum > maximum || default_value < minimum || default_value > maximum) {
            throw std::invalid_argument{"A numeric setting range must be ordered and contain its default"};
        }
        data_->has_range = true;
        data_->minimum = native_setting_value(minimum);
        data_->maximum = native_setting_value(maximum);
        return std::move(*this);
    }

    ValueSettingBuilder max_length(std::uint32_t byte_count) &&
        requires std::same_as<Value, std::string>
    {
        if (byte_count != 0 && data_->default_string.size() > byte_count) {
            throw std::invalid_argument{"A string setting default exceeds its declared maximum length"};
        }
        data_->max_length = byte_count;
        return std::move(*this);
    }

    operator SettingEntry<Settings>() const {
        return SettingEntry<Settings>{data_};
    }

  private:
    std::shared_ptr<SettingEntryData<Settings>> data_;
};

// Choice builders retain the author enum mapping while exposing only stable spellings and indices to the framework.
template <class Settings, class Enum> class ChoiceSettingBuilder {
  public:
    ChoiceSettingBuilder(std::string key, Enum Settings::* member, Enum default_value,
                         std::initializer_list<std::pair<std::string_view, Enum>> choices)
        : data_(std::make_shared<SettingEntryData<Settings>>()), values_(std::make_shared<std::vector<Enum>>()) {
        data_->key = std::move(key);
        data_->type = FC_SETTING_CHOICE;
        std::uint32_t default_index = 0;
        std::uint32_t index = 0;
        bool found_default = false;
        // Build parallel spelling and enum tables while locating the single index used by the native schema.
        for (const auto& [name, value] : choices) {
            if (name.empty()) {
                throw std::invalid_argument{"A choice setting cannot contain an empty spelling"};
            }
            if (std::ranges::any_of(data_->choice_names, [&](std::string_view existing) {
                    return equal_ascii_case_insensitive(existing, name);
                })) {
                throw std::invalid_argument{"Choice spellings must be unique under ASCII case-insensitive comparison"};
            }
            data_->choice_names.emplace_back(name);
            values_->push_back(value);
            if (value == default_value) {
                default_index = index;
                found_default = true;
            }
            ++index;
        }
        if (data_->choice_names.empty()) {
            throw std::invalid_argument{"A choice setting must declare at least one choice"};
        }
        if (!found_default) {
            throw std::invalid_argument{"A choice setting default must appear in its declared choices"};
        }
        data_->default_value.choice_index = default_index;
        // Assignment translates the stable index back to the author enum without exposing that enum to the framework.
        const auto values = values_;
        data_->assign = [member, values](Settings& settings, const FC_SettingValue& value) {
            if (value.choice_index < values->size()) {
                settings.*member = (*values)[value.choice_index];
            }
        };
    }

    ChoiceSettingBuilder description(std::string value) && {
        data_->description = std::move(value);
        return std::move(*this);
    }

    ChoiceSettingBuilder environment(std::string value) && {
        data_->environment = std::move(value);
        return std::move(*this);
    }

    operator SettingEntry<Settings>() const {
        return SettingEntry<Settings>{data_};
    }

  private:
    std::shared_ptr<SettingEntryData<Settings>> data_;
    std::shared_ptr<std::vector<Enum>> values_;
};

template <class Settings> struct SettingEntryAccess {
    static const std::shared_ptr<const SettingEntryData<Settings>>& data(const SettingEntry<Settings>& entry) {
        return entry.data_;
    }

    static SettingEntry<Settings> make(std::shared_ptr<const SettingEntryData<Settings>> data) {
        return SettingEntry<Settings>{std::move(data)};
    }
};

} // namespace detail

// Composition helpers retain schema order, eagerly validate defaults, and throw before registration for invalid input.
template <class Settings, class Value>
    requires detail::SettingValue<Value>
[[nodiscard]] auto value(std::string key, Value Settings::* member, Value default_value) {
    return detail::ValueSettingBuilder<Settings, Value>{std::move(key), member, std::move(default_value)};
}

template <class Settings, class Enum>
    requires std::is_enum_v<Enum>
[[nodiscard]] auto choice(std::string key, Enum Settings::* member, Enum default_value,
                          std::initializer_list<std::pair<std::string_view, Enum>> choices) {
    return detail::ChoiceSettingBuilder<Settings, Enum>{std::move(key), member, default_value, choices};
}

template <class Settings, class... Entries> [[nodiscard]] SettingsSchema<Settings> settings(Entries&&... entries) {
    SettingsSchema<Settings> result;
    (result.entries.emplace_back(static_cast<SettingEntry<Settings>>(std::forward<Entries>(entries))), ...);
    return result;
}

template <class Settings>
[[nodiscard]] SettingsSection<Settings> section(std::string id, std::initializer_list<SettingEntry<Settings>> entries) {
    return {.id = std::move(id), .entries = entries};
}

template <class Settings, std::ranges::input_range EntryRange>
    requires std::convertible_to<std::ranges::range_reference_t<EntryRange>, SettingEntry<Settings>>
[[nodiscard]] SettingsSection<Settings> section(std::string id, EntryRange&& entries) {
    SettingsSection<Settings> result{.id = std::move(id)};
    for (auto&& entry : entries) {
        result.entries.emplace_back(static_cast<SettingEntry<Settings>>(entry));
    }
    return result;
}

template <class Settings, std::ranges::input_range Declarations>
    requires std::same_as<std::remove_cvref_t<std::ranges::range_value_t<Declarations>>, StringSetting>
[[nodiscard]] SettingsSection<Settings> section(std::string id, StringMap Settings::* member,
                                                Declarations&& declarations) {
    SettingsSection<Settings> result{.id = std::move(id)};
    for (const StringSetting& declaration : declarations) {
        auto data = std::make_shared<detail::SettingEntryData<Settings>>();
        data->key = std::string{declaration.key};
        data->description = std::string{declaration.description};
        data->environment = std::string{declaration.environment};
        data->type = FC_SETTING_STRING;
        data->default_string = std::string{declaration.default_value};
        data->default_value.string_value = detail::string_view(data->default_string);
        data->max_length = declaration.max_length;
        const auto key = data->key;
        data->assign = [member, key](Settings& settings, const FC_SettingValue& value) {
            detail::StringMapAccess::assign(settings.*member, key,
                                            std::string{detail::string_view(value.string_value)});
        };
        result.entries.emplace_back(detail::SettingEntryAccess<Settings>::make(std::move(data)));
    }
    return result;
}

template <class Settings>
[[nodiscard]] SettingsSection<Settings> section(std::string id, StringMap Settings::* member,
                                                std::initializer_list<StringSetting> declarations) {
    return section(std::move(id), member, std::span{declarations.begin(), declarations.size()});
}

// Immutable author definitions are lowered through the SDK and copied synchronously during framework admission.
class Support;
class Patch;
class Plugin;

// Categories provide optional presentation order without changing registration or selection order.
struct CategoryDefinition {
    std::string id;
    std::optional<std::uint32_t> order;
};

// Groups organize owned patches for presentation and optional shared selection.
struct GroupDefinition {
    std::string id;
    std::vector<std::string> members;
    bool configurable = false;
    bool enabled = false;
    std::optional<std::string> category;
    std::optional<std::string> description;
};

// A support narrows one patch implementation to compatible target and configuration combinations.
template <class Settings = detail::NoSettings> struct SupportDefinition {
    std::vector<TargetLayout> layouts;
    HostRole roles{};
    TargetImage image{};
    std::optional<SettingsSchema<Settings>> settings;
    std::vector<std::string> depends_on;
    std::vector<std::string> includes;
    std::optional<FailurePolicy> failure_policy;
};

// Copyable type-erased support definition that keeps its typed settings and handler model alive.
class Support {
  public:
    Support(const Support&) = default;
    Support& operator=(const Support&) = default;
    Support(Support&&) noexcept = default;
    Support& operator=(Support&&) noexcept = default;

  private:
    explicit Support(std::shared_ptr<const detail::SupportConcept> implementation)
        : implementation_(std::move(implementation)) {}
    std::shared_ptr<const detail::SupportConcept> implementation_;

    template <class Handler> friend Support support(SupportDefinition<detail::SettingsFor<Handler>> definition);
    friend Support support(SupportDefinition<> definition);
    friend struct detail::PluginAccess;
    template <class Handler> friend class detail::PatchModel;
};

// Patch metadata and common settings are immutable inputs retained by the type-erased Patch.
template <class Settings = detail::NoSettings> struct PatchDefinition {
    std::string id;
    std::string name;
    bool enabled = false;
    bool configurable = true;
    std::optional<std::string> description;
    std::optional<std::string> version;
    std::optional<std::string> author;
    std::optional<std::string> source;
    std::optional<std::string> category;
    std::optional<SettingsSchema<Settings>> settings;
    std::vector<std::string> depends_on;
    std::vector<std::string> includes;
    FailurePolicy failure_policy = FailurePolicy::Continue;
    std::vector<Support> supports;
};

// Copyable type-erased patch definition; runtime instances are created later by its retained adapter.
class Patch {
  public:
    Patch(const Patch&) = default;
    Patch& operator=(const Patch&) = default;
    Patch(Patch&&) noexcept = default;
    Patch& operator=(Patch&&) noexcept = default;

  private:
    explicit Patch(std::shared_ptr<const detail::PatchConcept> implementation)
        : implementation_(std::move(implementation)) {}
    std::shared_ptr<const detail::PatchConcept> implementation_;

    template <class Handler> friend Patch patch(PatchDefinition<detail::SettingsFor<Handler>> definition);
    template <class PlanFunction>
    friend Patch plan_patch(
        PatchDefinition<typename detail::PlanCallableTraits<std::remove_cvref_t<PlanFunction>>::Settings> definition,
        PlanFunction plan);
    friend struct detail::PluginAccess;
};

// A plugin definition is the complete author-owned catalog contribution lowered during registration.
struct PluginDefinition {
    std::string id;
    std::optional<std::string> version;
    std::optional<std::string> author;
    std::optional<std::string> source;
    std::vector<CategoryDefinition> categories;
    std::vector<GroupDefinition> groups;
    std::vector<Patch> patches;
};

// Copyable owning plugin definition returned by a factory; registration retains it only after acceptance.
class Plugin {
  public:
    Plugin(const Plugin&) = default;
    Plugin& operator=(const Plugin&) = default;
    Plugin(Plugin&&) noexcept = default;
    Plugin& operator=(Plugin&&) noexcept = default;

  private:
    explicit Plugin(std::shared_ptr<const PluginDefinition> definition) : definition_(std::move(definition)) {}
    std::shared_ptr<const PluginDefinition> definition_;

    friend Plugin plugin(PluginDefinition definition);
    friend struct detail::PluginAccess;
};

// Validates immediately and throws invalid_argument for malformed author composition; the ABI adapter contains it.
[[nodiscard]] inline Plugin plugin(PluginDefinition definition) {
    if (!detail::valid_id(definition.id)) {
        throw std::invalid_argument{
            "A plugin ID must contain 1-64 ASCII letters, digits, or underscores and begin with a letter"};
    }
    if (detail::reserved_plugin_id(definition.id)) {
        throw std::invalid_argument{"FusionCutter is a reserved plugin ID"};
    }
    if (definition.patches.empty() && !detail::equal_ascii_case_insensitive(definition.id, "Core")) {
        throw std::invalid_argument{"A plugin must declare at least one patch"};
    }
    return Plugin{std::make_shared<const PluginDefinition>(std::move(definition))};
}

// Borrowed for lightweight handler construction; retain only the returned Logger or copied target facts.
class CreateContext {
  public:
    [[nodiscard]] TargetInfo target() const noexcept {
        return target_;
    }
    [[nodiscard]] Logger logger() const noexcept {
        return logger_;
    }

  private:
    CreateContext(TargetInfo target, Logger logger) noexcept : target_(target), logger_(logger) {}
    TargetInfo target_{};
    Logger logger_;
    friend class detail::ContextFactory;
};

// Borrowed for the fallible Prepare phase, where substantial resources and prepared bindings may be acquired.
class PrepareContext {
  public:
    [[nodiscard]] Logger logger() const noexcept {
        return logger_;
    }

    // A malformed, foreign, or unresolved handle returns an empty span and fails the enclosing Prepare phase.
    template <NativeData T> [[nodiscard]] std::span<T> resolve(DataHandle<T> handle) noexcept;

    // Returns a copied interface with the exact declared layout only from an active selected provider.
    template <InterfaceContract Interface>
    [[nodiscard]] std::optional<Interface> find_interface(std::string_view provider_patch) const noexcept;

    // A disabled channel is a successful inert handle; malformed or over-budget requests return Error.
    [[nodiscard]] std::expected<TraceChannel, Error> create_trace(TraceDefinition definition);

  private:
    PrepareContext(const FC_HostApi* host, const FC_PrepareContext* context) noexcept;
    const FC_HostApi* host_ = nullptr;
    const FC_PrepareContext* context_ = nullptr;
    Logger logger_;
    bool failed_ = false;
    template <class Handler> friend class detail::HandlerAdapter;
    friend class detail::ContextFactory;
};

// Borrowed for bounded, non-failing activation after the native Commit phase has succeeded.
class ActivateContext {
  public:
    [[nodiscard]] Logger logger() const noexcept {
        return logger_;
    }

  private:
    explicit ActivateContext(Logger logger) noexcept : logger_(logger) {}
    Logger logger_;
    friend class detail::ContextFactory;
};

// Borrowed for bounded, nonblocking work on the serialized loader pump.
class UpdateContext {
  public:
    [[nodiscard]] Logger logger() const noexcept {
        return logger_;
    }

  private:
    explicit UpdateContext(Logger logger) noexcept : logger_(logger) {}
    Logger logger_;
    friend class detail::ContextFactory;
};

namespace detail {

class ContextFactory {
  public:
    static Logger logger(const FC_HostApi* host, FC_ReportToken report) noexcept {
        return Logger{host, report};
    }
    static StatusWriter status(const FC_StatusSink* sink) noexcept {
        return StatusWriter{sink};
    }
    static TraceChannel trace(const FC_HostApi* host, FC_TraceHandle handle) noexcept {
        return TraceChannel{host, handle};
    }
    static CreateContext create(const FC_HostApi* host, const FC_CreateContext& context) noexcept;
    static PrepareContext prepare(const FC_HostApi* host, const FC_PrepareContext& context) noexcept {
        return PrepareContext{host, &context};
    }
    static ActivateContext activate(const FC_HostApi* host, const FC_ActivateContext& context) noexcept {
        return ActivateContext{Logger{host, context.report}};
    }
    static UpdateContext update(const FC_HostApi* host, const FC_UpdateContext& context) noexcept {
        return UpdateContext{Logger{host, context.report}};
    }
};

} // namespace detail

} // namespace fc

#include <FusionCutter/detail/adapter.hpp>
