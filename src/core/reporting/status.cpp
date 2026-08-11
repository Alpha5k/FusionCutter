#include "status.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>

namespace fusioncutter {
namespace {

template <std::size_t Capacity>
void copy_status_text(std::array<char, Capacity>& destination, std::size_t& destination_size,
                      std::string_view source) noexcept {
    destination_size = std::min(source.size(), destination.size());
    for (std::size_t index = 0; index < destination_size; ++index) {
        const char character = source[index];
        destination[index] = character == '\r' || character == '\n' ? ' ' : character;
    }
}

} // namespace

bool StatusSection::set(std::string_view label, std::string_view value) noexcept {
    if (label.empty() || size_ == fields_.size()) {
        return false;
    }

    auto& field = fields_[size_++];
    copy_status_text(field.label, field.label_size, label);
    copy_status_text(field.value, field.value_size, value);
    return label.size() <= field.label.size() && value.size() <= field.value.size();
}

} // namespace fusioncutter

namespace fusioncutter::reporting {
namespace {

constexpr std::size_t kMaximumStatusSize = 64 * 1024;
constexpr std::string_view kTruncationMarker = "\r\n[Status output truncated]\r\n";

class StatusText {
  public:
    void append(std::string_view value) {
        if (truncated_ || value.empty()) {
            return;
        }

        if (value.size() <= kMaximumStatusSize - text_.size()) {
            text_.append(value);
            return;
        }

        truncated_ = true;
        const auto content_capacity = kMaximumStatusSize - std::min(kMaximumStatusSize, kTruncationMarker.size());
        if (text_.size() < content_capacity) {
            text_.append(value.substr(0, content_capacity - text_.size()));
        } else {
            text_.resize(content_capacity);
        }
        text_.append(kTruncationMarker);
    }

    [[nodiscard]] std::string take() && {
        return std::move(text_);
    }

  private:
    std::string text_;
    bool truncated_{};
};

[[nodiscard]] StatusPublisher::StoredReason copy_reason(const OutcomeReason& reason,
                                                        std::span<const PatchResult> patch_results) {
    return {
        reason.message,
        reason.operation,
        reason.related_patch.transform([patch_results](PatchId patch_id) {
            const auto result = std::ranges::find(patch_results, patch_id, &PatchResult::patch_id);
            return std::string(result == patch_results.end() ? patch_id : result->name);
        }),
    };
}

[[nodiscard]] std::string_view initialization_name(InitializationOutcome outcome) noexcept {
    switch (outcome) {
    case InitializationOutcome::Completed:
        return "Completed";
    case InitializationOutcome::Unsupported:
        return "Unsupported";
    case InitializationOutcome::Fatal:
        return "Fatal";
    }
    return "Fatal";
}

[[nodiscard]] std::string_view layout_name(TargetLayout layout) noexcept {
    switch (layout) {
    case TargetLayout::SteamRetail:
        return "Steam";
    case TargetLayout::GOGRetail:
        return "GOG";
    case TargetLayout::Aspyr:
        return "Aspyr";
    case TargetLayout::ModTools:
        return "Mod Tools";
    }
    return "Unknown";
}

[[nodiscard]] std::string_view role_name(HostRole role) noexcept {
    return role == HostRole::Client ? "Client" : "Server";
}

[[nodiscard]] std::string_view architecture_name(Architecture architecture) noexcept {
    return architecture == Architecture::X86 ? "x86" : "x64";
}

void append_reason(StatusText& output, const StatusPublisher::StoredReason& reason, std::string_view indentation) {
    if (reason.operation.has_value()) {
        output.append(indentation);
        output.append("Operation: ");
        output.append(*reason.operation);
        output.append("\r\n");
    }
    output.append(indentation);
    output.append("Reason: ");
    output.append(reason.message);
    output.append("\r\n");
    if (reason.related_patch.has_value()) {
        output.append(indentation);
        output.append("Related patch: ");
        output.append(*reason.related_patch);
        output.append("\r\n");
    }
}

void debug_output(std::string_view message, DWORD error) noexcept {
    std::string text = "Fusion Cutter status output failed: ";
    text.append(message);
    if (error != ERROR_SUCCESS) {
        text.append(" (Windows error ");
        text.append(std::to_string(error));
        text.push_back(')');
    }
    text.append("\r\n");
    OutputDebugStringA(text.c_str());
}

void remove_file(const std::filesystem::path& path) noexcept {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

[[nodiscard]] bool write_status_file(const std::filesystem::path& path, const std::filesystem::path& temporary_path,
                                     std::string_view content) noexcept {
    try {
        std::ofstream output(temporary_path, std::ios::binary | std::ios::trunc);
        if (!output) {
            debug_output("the temporary file could not be opened", GetLastError());
            return false;
        }
        output.write(content.data(), static_cast<std::streamsize>(content.size()));
        output.close();
        if (!output) {
            debug_output("the temporary file could not be written", GetLastError());
            remove_file(temporary_path);
            return false;
        }

        if (!MoveFileExW(temporary_path.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            const auto error = GetLastError();
            debug_output("the snapshot could not replace the status file", error);
            remove_file(temporary_path);
            return false;
        }
        return true;
    } catch (...) {
        remove_file(temporary_path);
        debug_output("an unexpected file error occurred", ERROR_GEN_FAILURE);
        return false;
    }
}

} // namespace

StatusPublisher::StatusPublisher(std::filesystem::path path) : path_(std::move(path)), temporary_path_(path_) {
    temporary_path_.replace_extension(L".tmp");
}

void StatusPublisher::set_target(const TargetContext& target) {
    const std::scoped_lock lock(mutex_);
    target_ = target;
}

void StatusPublisher::set_configuration(const std::filesystem::path& path) {
    const std::scoped_lock lock(mutex_);
    configuration_ = path;
}

void StatusPublisher::set_snapshot(const InitializationResult& initialization,
                                   std::span<const PatchResult> patch_results,
                                   std::span<const StatusContributorRef> contributors) {
    const std::scoped_lock lock(mutex_);
    initialization_ = initialization.outcome;
    initialization_reason_ = initialization.reason.transform([patch_results](const auto& reason) {
        return copy_reason(reason, patch_results);
    });

    patch_results_.clear();
    patch_results_.reserve(patch_results.size());
    for (const auto& result : patch_results) {
        patch_results_.push_back({
            std::string(result.name),
            result.outcome,
            result.reason.transform([patch_results](const auto& reason) {
                return copy_reason(reason, patch_results);
            }),
        });
    }

    contributors_.clear();
    contributors_.reserve(contributors.size());
    for (const auto& contributor : contributors) {
        if (contributor.contributor != nullptr) {
            contributors_.push_back({std::string(contributor.name), contributor.contributor});
        }
    }
}

std::string StatusPublisher::render(const LogStatus& log_status) const {
    StatusText output;
    output.append("Fusion Cutter ");
    output.append(FC_VERSION_STRING);
    output.append("\r\nStatus: ");
    output.append(initialization_name(initialization_));
    output.append("\r\n");
    if (initialization_reason_.has_value()) {
        append_reason(output, *initialization_reason_, {});
    }

    output.append("Target: ");
    if (target_.has_value()) {
        output.append(layout_name(target_->layout));
        output.append(" (");
        output.append(role_name(target_->role));
        output.append(", ");
        output.append(architecture_name(target_->image.architecture));
        output.append(")\r\n");
    } else {
        output.append("Unrecognized\r\n");
    }

    output.append("Configuration: ");
    output.append(configuration_.has_value() ? configuration_->filename().string() : "Not loaded");
    output.append("\r\nLog: ");
    if (log_status.output_failed) {
        output.append("Output unavailable");
    } else if (log_status.accepted_entries != 0 || log_status.written_entries != 0) {
        output.append(log_status.filename.filename().string());
    } else if (log_status.level == LogLevel::Off) {
        output.append("Logging disabled");
    } else {
        std::error_code error;
        const bool old_log_exists = std::filesystem::exists(log_status.filename, error);
        output.append(old_log_exists && !error ? "No entries this session" : "No log created");
    }
    output.append("\r\n");

    constexpr std::array groups{
        std::pair{PatchOutcome::WaitingForImage, std::string_view{"Waiting for image:"}},
        std::pair{PatchOutcome::Installed, std::string_view{"Installed patches:"}},
        std::pair{PatchOutcome::Skipped, std::string_view{"Skipped patches:"}},
        std::pair{PatchOutcome::Failed, std::string_view{"Failed patches:"}},
    };
    for (const auto& [outcome, heading] : groups) {
        const bool has_results = std::ranges::any_of(patch_results_, [&](const auto& result) {
            return result.outcome == outcome;
        });
        if (!has_results) {
            continue;
        }

        output.append("\r\n");
        output.append(heading);
        output.append("\r\n");
        for (const auto& result : patch_results_) {
            if (result.outcome != outcome) {
                continue;
            }
            output.append("  ");
            output.append(result.name);
            output.append("\r\n");
            if (result.reason.has_value()) {
                append_reason(output, *result.reason, "    ");
            }
        }
    }

    for (const auto& contributor : contributors_) {
        StatusSection section;
        contributor.contributor->write_status(section);
        if (section.size_ == 0) {
            continue;
        }

        output.append("\r\n");
        output.append(contributor.name);
        output.append(":\r\n");
        for (std::size_t index = 0; index < section.size_; ++index) {
            const auto& field = section.fields_[index];
            output.append("  ");
            output.append(std::string_view(field.label.data(), field.label_size));
            output.append(": ");
            output.append(std::string_view(field.value.data(), field.value_size));
            output.append("\r\n");
        }
    }
    return std::move(output).take();
}

void StatusPublisher::publish(const LogStatus& log_status, bool force) noexcept {
    try {
        const std::scoped_lock lock(mutex_);
        const auto now = std::chrono::steady_clock::now();
        if (!force && now - last_publication_ < std::chrono::seconds(1)) {
            return;
        }
        last_publication_ = now;

        auto output = render(log_status);
        if (output == previous_output_) {
            return;
        }
        if (write_status_file(path_, temporary_path_, output)) {
            previous_output_ = std::move(output);
        }
    } catch (...) {
        debug_output("the snapshot could not be rendered", ERROR_GEN_FAILURE);
    }
}

void StatusPublisher::publish_now(const LogStatus& log_status) noexcept {
    publish(log_status, true);
}

void StatusPublisher::publish_if_due(const LogStatus& log_status) noexcept {
    publish(log_status, false);
}

} // namespace fusioncutter::reporting
