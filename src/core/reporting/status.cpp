#include "status.hpp"

#include "../catalog/definition_copy.hpp"
#include "../runtime/patch_runtime.hpp"

#include <FusionCutter/CoreApi.h>

#include <Windows.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstring>
#include <format>
#include <limits>
#include <ranges>
#include <utility>

namespace fc::reporting {
namespace {

inline constexpr std::size_t kMaximumStatusBytes = 128U * 1024U;
inline constexpr std::size_t kMaximumLiveSectionBytes = 4096;
inline constexpr std::size_t kLiveOmissionReserve = 64;
inline constexpr std::string_view kFileTruncation =
    "\r\n[Status output truncated; see FusionCutter.log for complete diagnostics]\r\n";

// StatusText preserves a complete truncation notice and never cuts through a UTF-8 continuation sequence.
class StatusText final {
  public:
    explicit StatusText(std::size_t capacity = kMaximumStatusBytes) : capacity_(capacity) {
        text_.reserve(std::min<std::size_t>(capacity, 8192));
    }

    void append(std::string_view value) {
        if (truncated_ || value.empty()) {
            return;
        }
        // Complete values take the direct path; truncation reserves the tail notice before copying partial content.
        if (value.size() <= capacity_ - text_.size()) {
            text_.append(value);
            return;
        }

        truncated_ = true;
        const auto marker_size = std::min(capacity_, kFileTruncation.size());
        const auto content_limit = capacity_ - marker_size;
        if (text_.size() > content_limit) {
            text_.resize(utf8_boundary(text_, content_limit));
        } else {
            const auto available = content_limit - text_.size();
            text_.append(value.substr(0, utf8_boundary(value, available)));
        }
        text_.append(kFileTruncation.substr(kFileTruncation.size() - marker_size));
    }

    // Structured text is normalized directly into this bounded destination so hostile metadata cannot allocate a
    // second unbounded copy before the limit for the complete file is applied.
    void append_structured(std::string_view value) {
        if (truncated_ || value.empty()) {
            return;
        }
        const auto marker_size = std::min(capacity_, kFileTruncation.size());
        const auto fits = value.size() <= capacity_ - text_.size();
        const auto available =
            fits ? value.size() : (text_.size() < capacity_ - marker_size ? capacity_ - marker_size - text_.size() : 0);
        const auto copied = fits ? value.size() : utf8_boundary(value, available);
        const auto offset = text_.size();
        text_.resize(offset + copied);
        // Normalize control bytes during the bounded copy so all status fields remain single structured lines.
        for (std::size_t index = 0; index < copied; ++index) {
            const auto byte = static_cast<unsigned char>(value[index]);
            text_[offset + index] = byte <= 0x1fU || byte == 0x7fU ? ' ' : value[index];
        }
        if (!fits) {
            // Replace only a complete UTF-8 suffix with the shared marker when the complete value cannot fit.
            truncated_ = true;
            if (text_.size() > capacity_ - marker_size) {
                text_.resize(utf8_boundary(text_, capacity_ - marker_size));
            }
            text_.append(kFileTruncation.substr(kFileTruncation.size() - marker_size));
        }
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return text_.size();
    }

    [[nodiscard]] std::string take() && {
        return std::move(text_);
    }

  private:
    [[nodiscard]] static std::size_t utf8_boundary(std::string_view value, std::size_t limit) noexcept {
        auto result = std::min(limit, value.size());
        while (result != 0 && result < value.size() && (static_cast<unsigned char>(value[result]) & 0xc0U) == 0x80U) {
            --result;
        }
        return result;
    }

    std::string text_;
    std::size_t capacity_{};
    bool truncated_{};
};

// Live fields have already passed their 4096-byte charge before this bounded normalized copy is made.
[[nodiscard]] std::string structured_line(std::string_view value) {
    std::string result{value};
    std::ranges::replace_if(
        result,
        [](char character) {
            const auto byte = static_cast<unsigned char>(character);
            return byte <= 0x1fU || byte == 0x7fU;
        },
        ' ');
    return result;
}

[[nodiscard]] bool valid_label(std::string_view label) noexcept {
    return !label.empty() && catalog::valid_utf8(label) && std::ranges::none_of(label, [](char character) {
        const auto byte = static_cast<unsigned char>(character);
        return character == ':' || byte <= 0x1fU || byte == 0x7fU;
    });
}

[[nodiscard]] int ordinal_compare(std::string_view left, std::string_view right) noexcept {
    const auto fold = [](unsigned char value) noexcept {
        return value >= 'A' && value <= 'Z' ? static_cast<unsigned char>(value + ('a' - 'A')) : value;
    };
    const auto shared = std::min(left.size(), right.size());
    for (std::size_t index = 0; index < shared; ++index) {
        const auto left_byte = fold(static_cast<unsigned char>(left[index]));
        const auto right_byte = fold(static_cast<unsigned char>(right[index]));
        if (left_byte != right_byte) {
            return left_byte < right_byte ? -1 : 1;
        }
    }
    return left.size() == right.size() ? 0 : (left.size() < right.size() ? -1 : 1);
}

[[nodiscard]] std::string_view initialization_name(InitializationStatus value) noexcept {
    switch (value) {
    case InitializationStatus::Completed:
        return "Completed";
    case InitializationStatus::Unsupported:
        return "Unsupported";
    case InitializationStatus::Fatal:
        return "Fatal";
    }
    return "Fatal";
}

[[nodiscard]] std::string_view phase_name(planning::PatchPhase value) noexcept {
    switch (value) {
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

[[nodiscard]] std::string_view layout_name(FC_TargetLayout value) noexcept {
    switch (value) {
    case FC_LAYOUT_GAMESPY_RETAIL:
        return "GameSpy Retail";
    case FC_LAYOUT_STEAM_RETAIL:
        return "Steam Retail";
    case FC_LAYOUT_GOG_RETAIL:
        return "GOG Retail";
    case FC_LAYOUT_MOD_TOOLS:
        return "Mod Tools";
    case FC_LAYOUT_CLASSIC_COLLECTION:
        return "Classic Collection";
    default:
        return "Unknown";
    }
}

[[nodiscard]] std::string_view role_name(FC_HostRole value) noexcept {
    return value == FC_HOST_ROLE_SERVER ? "Server" : "Client";
}

[[nodiscard]] std::string_view architecture_name(FC_Architecture value) noexcept {
    return value == FC_ARCH_X64 ? "x64" : "x86";
}

[[nodiscard]] std::string_view image_name(FC_TargetImage value) noexcept {
    switch (value) {
    case FC_IMAGE_GAME:
        return "Game";
    case FC_IMAGE_BOOTSTRAP:
        return "Bootstrap";
    case FC_IMAGE_GALAXY_PEER:
        return "GalaxyPeer";
    default:
        return "Unknown";
    }
}

[[nodiscard]] std::string_view log_level_name(FC_LogLevel value) noexcept {
    switch (value) {
    case FC_LOG_ERROR:
        return "Error";
    case FC_LOG_WARNING:
        return "Warning";
    case FC_LOG_INFO:
        return "Info";
    case FC_LOG_DEBUG:
        return "Debug";
    default:
        return "Disabled";
    }
}

[[nodiscard]] std::string local_started(std::chrono::system_clock::time_point started) {
    // Convert the C++ epoch to Windows FILETIME so the displayed offset follows the machine's current time-zone rules.
    const auto ticks = std::chrono::duration_cast<std::chrono::duration<std::int64_t, std::ratio<1, 10'000'000>>>(
                           started.time_since_epoch())
                           .count() +
                       116'444'736'000'000'000LL;
    FILETIME utc_file{static_cast<DWORD>(ticks), static_cast<DWORD>(static_cast<std::uint64_t>(ticks) >> 32U)};
    SYSTEMTIME utc{};
    SYSTEMTIME local{};
    FileTimeToSystemTime(&utc_file, &utc);
    SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local);

    TIME_ZONE_INFORMATION zone{};
    const auto state = GetTimeZoneInformation(&zone);
    LONG bias = zone.Bias;
    if (state == TIME_ZONE_ID_STANDARD) {
        bias += zone.StandardBias;
    } else if (state == TIME_ZONE_ID_DAYLIGHT) {
        bias += zone.DaylightBias;
    }
    const auto offset_minutes = -bias;
    return std::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02} {:+03}:{:02}", local.wYear, local.wMonth, local.wDay,
                       local.wHour, local.wMinute, local.wSecond, offset_minutes / 60, std::abs(offset_minutes % 60));
}

void append_reason(StatusText& output, const planning::FailureReason& reason, std::string_view indentation) {
    // Failure fields use one stable schema whether the failure belongs to initialization or an individual patch.
    if (reason.phase) {
        output.append(indentation);
        output.append("Phase: ");
        output.append(phase_name(*reason.phase));
        output.append("\r\n");
    }
    if (reason.operation) {
        output.append(indentation);
        output.append("Operation: ");
        output.append_structured(*reason.operation);
        output.append("\r\n");
    }
    output.append(indentation);
    output.append("Reason: ");
    output.append_structured(reason.message);
    output.append("\r\n");
    if (reason.related_patch) {
        output.append(indentation);
        output.append("Related patch: ");
        output.append_structured(*reason.related_patch);
        output.append("\r\n");
    }
    if (reason.related_group) {
        output.append(indentation);
        output.append("Related group: ");
        output.append_structured(*reason.related_group);
        output.append("\r\n");
    }
}

void debug_status_failure(std::string_view message, DWORD error) noexcept {
    try {
        std::array<char, 512> output{};
        const auto rendered =
            std::format_to_n(output.data(), output.size() - 1,
                             "Fusion Cutter status output failed: {} (Windows error {})\r\n", message, error);
        output[std::min<std::size_t>(rendered.size, output.size() - 1)] = '\0';
        OutputDebugStringA(output.data());
    } catch (...) {
        // Debug diagnostics are the fallback for failed status output, so their formatting must also be contained.
        OutputDebugStringA("Fusion Cutter status output failed while producing a diagnostic.\r\n");
    }
}

[[nodiscard]] bool write_status(const std::filesystem::path& path, std::string_view content) noexcept {
    // The status file is a current snapshot; each successful attempt replaces it in one bounded direct write.
    const auto file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        debug_status_failure("FusionCutter.txt could not be opened", GetLastError());
        return false;
    }
    DWORD written{};
    const auto size = static_cast<DWORD>(content.size());
    const bool success = WriteFile(file, content.data(), size, &written, nullptr) != 0 && written == size;
    const auto error = success ? ERROR_SUCCESS : GetLastError();
    CloseHandle(file);
    if (!success) {
        debug_status_failure("FusionCutter.txt could not be written", error);
    }
    return success;
}

// The sink charges rendered bytes, not arbitrary field counts, so small useful fields can consume the whole budget.
struct LiveCollector {
    LiveStatusSection* section{};
    std::size_t used{};

    [[nodiscard]] bool add(std::string_view label, std::string_view value) noexcept {
        // Invalid author data remains visible through the one omission count without entering structured output.
        if (!valid_label(label) || !catalog::valid_utf8(value)) {
            ++section->omitted;
            return false;
        }
        try {
            constexpr std::size_t decoration = 6;
            constexpr auto field_budget = kMaximumLiveSectionBytes - kLiveOmissionReserve;
            if (used >= field_budget) {
                ++section->omitted;
                return false;
            }
            const auto remaining = field_budget - used;
            if (label.size() > remaining || decoration > remaining - label.size() ||
                value.size() > remaining - label.size() - decoration) {
                ++section->omitted;
                return false;
            }
            // Charge the exact normalized field before committing it to the callback's bounded copied section.
            auto normalized = structured_line(value);
            const auto charge = decoration + label.size() + normalized.size();
            section->fields.push_back({std::string{label}, std::move(normalized)});
            used += charge;
            return true;
        } catch (...) {
            ++section->omitted;
            return false;
        }
    }
};

[[nodiscard]] std::optional<std::string_view> native_view(FC_StringView value) noexcept {
    if (value.data == nullptr && value.size != 0) {
        return std::nullopt;
    }
    return std::string_view{value.data == nullptr ? "" : value.data, value.size};
}

// Every rejected addition with a valid sink context contributes to the section's single omission count.
void omit_live_field(void* context) noexcept {
    if (auto* collector = static_cast<LiveCollector*>(context); collector != nullptr) {
        ++collector->section->omitted;
    }
}

// These ABI callbacks validate and charge plugin live fields before they enter framework-owned status storage.
FC_Bool FC_CALL add_text(void* context, FC_StringView label, FC_StringView value) {
    const auto label_view = native_view(label);
    const auto value_view = native_view(value);
    if (context == nullptr || !label_view || !value_view) {
        omit_live_field(context);
        return FC_FALSE;
    }
    return static_cast<LiveCollector*>(context)->add(*label_view, *value_view) ? FC_TRUE : FC_FALSE;
}

template <class Value> FC_Bool add_number(void* context, FC_StringView label, Value value) noexcept {
    const auto label_view = native_view(label);
    std::array<char, 96> text{};
    const auto converted = std::to_chars(text.data(), text.data() + text.size(), value);
    if (context == nullptr || !label_view || converted.ec != std::errc{}) {
        omit_live_field(context);
        return FC_FALSE;
    }
    return static_cast<LiveCollector*>(context)->add(*label_view, {text.data(), converted.ptr}) ? FC_TRUE : FC_FALSE;
}

FC_Bool FC_CALL add_signed(void* context, FC_StringView label, std::int64_t value) {
    return add_number(context, label, value);
}

FC_Bool FC_CALL add_unsigned(void* context, FC_StringView label, std::uint64_t value) {
    return add_number(context, label, value);
}

FC_Bool FC_CALL add_floating(void* context, FC_StringView label, double value) {
    if (!std::isfinite(value)) {
        omit_live_field(context);
        return FC_FALSE;
    }
    return add_number(context, label, value);
}

FC_Bool FC_CALL add_boolean(void* context, FC_StringView label, FC_Bool value) {
    const auto label_view = native_view(label);
    if (context == nullptr || !label_view || (value != FC_FALSE && value != FC_TRUE)) {
        omit_live_field(context);
        return FC_FALSE;
    }
    return static_cast<LiveCollector*>(context)->add(*label_view, value == FC_TRUE ? "true" : "false") ? FC_TRUE
                                                                                                       : FC_FALSE;
}

[[nodiscard]] const runtime::RetainedFailureRecord* retained_failure(const runtime::PatchRuntimeState& runtime,
                                                                     catalog::PatchIndex patch) noexcept {
    const auto found = std::ranges::find(runtime.retained_failures, patch, &runtime::RetainedFailureRecord::patch);
    return found == runtime.retained_failures.end() ? nullptr : &*found;
}

} // namespace

StatusPublisher::StatusPublisher(std::filesystem::path path) : path_(std::move(path)) {}

void StatusPublisher::set_session(SessionFacts facts) {
    session_ = std::move(facts);
}

void StatusPublisher::set_target(const targets::RecognizedTarget& target) {
    target_ = &target;
}

void StatusPublisher::set_catalog(const catalog::Catalog& catalog, std::span<const catalog::RejectionRecord> rejections,
                                  std::filesystem::path core_configuration) {
    catalog_ = &catalog;
    rejections_.assign(rejections.begin(), rejections.end());
    core_configuration_ = std::move(core_configuration);
}

std::vector<LiveStatusSection> StatusPublisher::collect_live(const runtime::PatchRuntimeState& runtime) const {
    // Only installed instances may contribute live state; absent callbacks produce no empty status sections.
    std::vector<LiveStatusSection> result;
    result.reserve(runtime.installed_patches.size());
    for (const auto& installed : runtime.installed_patches) {
        const auto callback = installed.instance.callbacks().write_status;
        if (callback == nullptr) {
            continue;
        }

        LiveStatusSection section{.patch = installed.patch};
        const auto& definition = runtime.catalog.patch(installed.patch);
        const auto plugin = runtime.catalog.patch_plugin(installed.patch);
        const auto& plugin_id = runtime.catalog.plugin(plugin).definition.id;
        LiveCollector collector{&section, definition.name.size() + plugin_id.size() + definition.id.size() + 12};
        const FC_StatusSink sink{.struct_size = sizeof(FC_StatusSink),
                                 .context = &collector,
                                 .add_text = &add_text,
                                 .add_signed = &add_signed,
                                 .add_unsigned = &add_unsigned,
                                 .add_floating = &add_floating,
                                 .add_boolean = &add_boolean};
        // A throwing plugin loses only this snapshot contribution and is recorded as omitted diagnostic data.
        try {
            callback(installed.instance.callbacks().context, installed.instance.get(), &sink);
        } catch (...) {
            ++section.omitted;
            section.callback_failed = true;
        }
        if (!section.fields.empty() || section.omitted != 0) {
            result.push_back(std::move(section));
        }
    }
    // Stable ordering by patch ID keeps publication of changed state and human comparisons deterministic.
    std::ranges::sort(result, [&](const auto& left, const auto& right) {
        return ordinal_compare(runtime.catalog.patch(left.patch).id, runtime.catalog.patch(right.patch).id) < 0;
    });
    return result;
}

bool StatusPublisher::publication_due(bool force) const noexcept {
    return force || last_attempt_ == std::chrono::steady_clock::time_point{} ||
           std::chrono::steady_clock::now() - last_attempt_ >= std::chrono::seconds{1};
}

std::string StatusPublisher::render(InitializationStatus initialization, const planning::FailureReason* reason,
                                    const runtime::PatchRuntimeState* runtime, std::span<const LiveStatusSection> live,
                                    const LogStatus& log, const TraceStatus& trace) const {
    StatusText output;
    // The opening summary remains useful before target recognition or catalog admission has completed.
    output.append("Fusion Cutter ");
    output.append(FC_VERSION_STRING);
    if (std::string_view{FC_BUILD_ID} != "" && std::string_view{FC_BUILD_ID} != FC_VERSION_STRING) {
        output.append(" (");
        output.append(FC_BUILD_ID);
        output.append(")");
    }
    output.append("\r\nStarted: ");
    output.append(session_ ? local_started(session_->started) : "Unknown");
    output.append("\r\nInitialization: ");
    output.append(initialization_name(initialization));
    output.append("\r\n");
    if (reason != nullptr && initialization != InitializationStatus::Completed) {
        append_reason(output, *reason, {});
    }

    // Target and output facts stay useful even when startup ends before a plugin catalog becomes available.
    output.append("Target: ");
    if (target_ != nullptr) {
        output.append(layout_name(target_->layout()));
        output.append(" (");
        output.append(role_name(target_->role()));
        output.append(", ");
        output.append(architecture_name(target_->architecture()));
        output.append(")\r\n");
    } else {
        output.append("Unrecognized");
        if (session_ && !session_->executable_basename.empty()) {
            output.append(" (");
            output.append_structured(session_->executable_basename);
            if (session_->executable_timestamp && session_->executable_image_size) {
                output.append("; timestamp=");
                output.append(std::format("0x{:08x}", *session_->executable_timestamp));
                output.append("; image-size=");
                output.append(std::format("0x{:x}", *session_->executable_image_size));
            }
            output.append(")");
        }
        output.append("\r\n");
    }
    output.append("Configuration: ");
    output.append(core_configuration_.empty() ? "Not loaded" : core_configuration_.string());
    output.append("\r\nLog: ");
    if (log.output_failed) {
        output.append("Unavailable");
    } else if (log.level == FC_LOG_OFF) {
        output.append("Disabled");
    } else {
        output.append(log_level_name(log.level));
        output.append(", ");
        output.append(log.written == 0 ? "no file this session" : log.path.filename().string());
    }
    if (log.dropped != 0) {
        output.append(std::format(", {} dropped", log.dropped));
    }
    output.append("\r\n");

    // Optional diagnostic and proxy facts appear only after their subsystem or loader route participates.
    if (trace.requested) {
        output.append("Trace: ");
        if (trace.configured_disabled) {
            output.append("Disabled by configuration");
        } else if (trace.output_failed) {
            output.append("Unavailable");
        } else if (trace.file_limit_reached) {
            output.append("File limit reached");
        } else if (trace.path) {
            output.append(trace.path->parent_path().filename().string());
            output.append("\\");
            output.append(trace.path->filename().string());
        } else {
            output.append("Awaiting first record");
        }
        if (trace.dropped != 0) {
            output.append(std::format(", {} dropped", trace.dropped));
        }
        output.append("\r\n");
    }
    if (session_ && session_->loader_kind == FC_LOADER_KIND_DINPUT8 &&
        session_->direct_input_chain != FC_DIRECT_INPUT_CHAIN_SYSTEM_ONLY) {
        output.append("DirectInput proxy: ");
        if (session_->selected_proxy_basename.empty()) {
            output.append("Unavailable");
        } else {
            output.append_structured(session_->selected_proxy_basename);
        }
        output.append("\r\n");
    }

    if (catalog_ != nullptr) {
        // Installed plugin identities are independent of discovery order, with the built-in Core plugin shown first.
        std::vector<const catalog::PluginRecord*> plugins;
        plugins.reserve(catalog_->plugins().size());
        for (const auto& plugin : catalog_->plugins()) {
            plugins.push_back(&plugin);
        }
        std::ranges::sort(plugins, [](const auto* left, const auto* right) {
            if (left->definition.id == "Core" || right->definition.id == "Core") {
                return left->definition.id == "Core" && right->definition.id != "Core";
            }
            return ordinal_compare(left->definition.id, right->definition.id) < 0;
        });
        if (!plugins.empty()) {
            output.append("\r\nInstalled plugins:\r\n");
            for (const auto* plugin : plugins) {
                output.append("  ");
                output.append_structured(plugin->definition.id);
                if (!plugin->definition.version.empty()) {
                    output.append(" (");
                    output.append_structured(plugin->definition.version);
                    output.append(")");
                }
                output.append("\r\n");
            }
        }
    }

    if (!rejections_.empty()) {
        // Bundled plugin rejections precede path-backed candidates so failures inside FusionCutter.dll stay prominent.
        output.append("\r\nRejected plugins:\r\n");
        std::vector<const catalog::RejectionRecord*> bundled;
        std::vector<const catalog::RejectionRecord*> external;
        for (const auto& rejection : rejections_) {
            (rejection.path ? external : bundled).push_back(&rejection);
        }
        std::ranges::sort(bundled, [](const auto* left, const auto* right) {
            return ordinal_compare(left->plugin_id.value_or(""), right->plugin_id.value_or("")) < 0;
        });
        const auto append_rejection = [&](const catalog::RejectionRecord& rejection) {
            output.append("  ");
            output.append(rejection.path ? rejection.path->filename().string()
                                         : rejection.plugin_id.value_or("Bundled plugin") + " (bundled)");
            output.append("\r\n");
            if (rejection.plugin_id) {
                output.append("    Plugin ID: ");
                output.append_structured(*rejection.plugin_id);
                output.append("\r\n");
            }
            if (!rejection.operation.empty()) {
                output.append("    Operation: ");
                output.append_structured(rejection.operation);
                output.append("\r\n");
            }
            output.append("    Reason: ");
            output.append_structured(rejection.reason);
            output.append("\r\n");
        };
        for (const auto* rejection : bundled) {
            append_rejection(*rejection);
        }
        for (const auto* rejection : external) {
            append_rejection(*rejection);
        }
    }

    if (runtime != nullptr) {
        // Terminal and waiting outcomes are grouped by severity while each group retains deterministic patch order.
        struct Group {
            planning::PatchState state;
            std::string_view heading;
        };
        constexpr std::array groups{Group{planning::PatchState::Failed, "Failed patches:"},
                                    Group{planning::PatchState::Skipped, "Skipped patches:"},
                                    Group{planning::PatchState::WaitingForImage, "Waiting for image:"}};
        std::vector<const planning::PatchWorkRecord*> records;
        records.reserve(runtime->patches.records().size());
        for (const auto& record : runtime->patches.records()) {
            records.push_back(&record);
        }
        std::ranges::sort(records, [&](const auto* left, const auto* right) {
            return ordinal_compare(runtime->catalog.patch(left->patch).id, runtime->catalog.patch(right->patch).id) < 0;
        });
        for (const auto& group : groups) {
            if (!std::ranges::any_of(records, [&](const auto* record) {
                    return record->state == group.state;
                })) {
                continue;
            }
            output.append("\r\n");
            output.append(group.heading);
            output.append("\r\n");
            for (const auto* record : records) {
                if (record->state != group.state) {
                    continue;
                }
                const auto& definition = runtime->catalog.patch(record->patch);
                const auto& plugin = runtime->catalog.plugin(runtime->catalog.patch_plugin(record->patch));
                output.append("  ");
                output.append_structured(definition.name);
                output.append(" (");
                output.append(plugin.definition.id);
                output.append("/");
                output.append(definition.id);
                output.append(")\r\n");
                if (record->state == planning::PatchState::WaitingForImage && definition.selected_support) {
                    output.append("    Image: ");
                    output.append(image_name(definition.supports[*definition.selected_support].image));
                    output.append("\r\n");
                }
                if (record->reason) {
                    append_reason(output, *record->reason, "    ");
                }
                if (const auto* retained = retained_failure(*runtime, record->patch); retained != nullptr) {
                    output.append("    Native state: ");
                    output.append(retained->rollback == patching::RollbackResult::Residual
                                      ? "Residual; resources retained\r\n"
                                      : "Restored; resources retained\r\n");
                }
            }
        }

        // Each plugin's live section receives a hard limit before joining the limit for the complete file.
        for (const auto& section : live) {
            const auto& definition = runtime->catalog.patch(section.patch);
            const auto& plugin = runtime->catalog.plugin(runtime->catalog.patch_plugin(section.patch));
            StatusText rendered{kMaximumLiveSectionBytes};
            rendered.append("\r\n");
            rendered.append_structured(definition.name);
            rendered.append(" (");
            rendered.append(plugin.definition.id);
            rendered.append("/");
            rendered.append(definition.id);
            rendered.append("):\r\n");
            for (const auto& field : section.fields) {
                rendered.append("  ");
                rendered.append(field.label);
                rendered.append(": ");
                rendered.append(field.value);
                rendered.append("\r\n");
            }
            if (section.omitted != 0) {
                rendered.append(std::format("  [{} status field(s) omitted]\r\n", section.omitted));
            }
            output.append(std::move(rendered).take());
        }
    }
    return std::move(output).take();
}

StatusPublishResult StatusPublisher::publish(InitializationStatus initialization, const planning::FailureReason* reason,
                                             const runtime::PatchRuntimeState* runtime,
                                             std::span<const LiveStatusSection> live, const LogStatus& log,
                                             const TraceStatus& trace, bool force) noexcept {
    try {
        // Periodic opportunities are rate-limited; forced startup/fatal transitions bypass the cadence gate.
        const auto now = std::chrono::steady_clock::now();
        if (!force && last_attempt_ != std::chrono::steady_clock::time_point{} &&
            now - last_attempt_ < std::chrono::seconds{1}) {
            return StatusPublishResult::Deferred;
        }
        last_attempt_ = now;
        auto current = render(initialization, reason, runtime, live, log, trace);
        // Unchanged snapshots avoid needless file replacement, timestamp churn, and antivirus/file-watcher work.
        if (current == last_successful_output_) {
            return StatusPublishResult::Unchanged;
        }
        // The remembered snapshot advances only after a complete direct replacement succeeds.
        if (write_status(path_, current)) {
            last_successful_output_ = std::move(current);
            return StatusPublishResult::Written;
        }
        return StatusPublishResult::Failed;
    } catch (...) {
        debug_status_failure("the bounded snapshot could not be rendered", ERROR_GEN_FAILURE);
        return StatusPublishResult::Failed;
    }
}

} // namespace fc::reporting
