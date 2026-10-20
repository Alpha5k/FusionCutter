#include "status.hpp"

#include "bounded_text.hpp"

#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <format>
#include <utility>

namespace fc::reporting {

namespace {

constexpr std::size_t kMaximumStatusBytes = 128U * 1024U;
constexpr std::string_view kTruncation = "\n[Status report truncated]\n";

} // namespace

bool StatusText::append(std::string_view value) {
    if (truncated_) {
        return false;
    }

    if (value.size() <= kMaximumStatusBytes - text_.size()) {
        const auto start = text_.size();
        text_.resize(start + value.size());
        copy_structured_bytes(std::span{text_}.subspan(start), value);
        return true;
    }

    // Reserving the marker may shorten a preceding line; both cuts stop at complete UTF-8 characters.
    const auto limit = kMaximumStatusBytes - kTruncation.size();
    if (text_.size() > limit) {
        text_.resize(utf8_prefix(text_, limit));
    }

    const auto count = utf8_prefix(value, limit - text_.size());
    const auto start = text_.size();
    text_.resize(start + count);
    copy_structured_bytes(std::span{text_}.subspan(start), value.substr(0, count));
    text_.append(kTruncation);
    truncated_ = true;
    return false;
}

bool StatusText::line(std::string_view value) {
    if (!append(value)) {
        return false;
    }

    // Add the line separator after normalizing the supplied text.
    if (text_.size() < kMaximumStatusBytes) {
        text_.push_back('\n');
        return true;
    }
    return append("\n");
}

std::uint32_t FC_CDECL StatusText::write_line(void *context, FC_Text value) {
    if (!value.data && value.size) {
        return 0;
    }

    try {
        auto &status = *static_cast<StatusText *>(context);
        std::string_view remaining{value.data ? value.data : "", value.size};
        // Split author-supplied line breaks before normalization so every submitted line can stay under its owner.
        do {
            const auto end = remaining.find_first_of("\r\n");
            if (!status.line(remaining.substr(0, end))) {
                return 0;
            }
            if (end == std::string_view::npos) {
                break;
            }
            const auto separator = remaining[end] == '\r' && end + 1 < remaining.size() && remaining[end + 1] == '\n' ? 2U : 1U;
            remaining.remove_prefix(end + separator);
        } while (!remaining.empty());
        return 1;
    } catch (...) {
        return 0;
    }
}

FC_StatusWriter StatusText::writer() noexcept {
    return {this, &write_line};
}

StatusPublisher::StatusPublisher(std::filesystem::path path) : path_(std::move(path)) {}

StatusPublishResult StatusPublisher::publish(std::string_view content) noexcept {
    if (successful_ && content == *successful_) {
        return {StatusPublishOutcome::Unchanged};
    }

    try {
        // Add the publication time after comparing report contents, preserving timestamps for unchanged reports.
        const auto updated =
            std::format("Updated: {:%Y-%m-%d %H:%M:%S} UTC\n", std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));
        std::string rendered;

        // Include the timestamp in the file's byte limit while preserving the collected line separators.
        const auto limit = kMaximumStatusBytes - updated.size();
        if (content.size() <= limit) {
            rendered.append(content);
        } else {
            rendered.append(content.substr(0, utf8_prefix(content, limit - kTruncation.size())));
            rendered.append(kTruncation);
        }

        // Keep the header together while adding Updated only after semantic comparison.
        const auto started = rendered.find("Started: ");
        const auto separator = started == std::string::npos ? std::string::npos : rendered.find('\n', started);
        rendered.insert(separator == std::string::npos ? 0 : separator + 1, updated);

        const auto file = CreateFileW(path_.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                      OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            return {StatusPublishOutcome::Failed, "Could not open the FusionCutter.txt status report", GetLastError()};
        }

        LARGE_INTEGER offset{};
        DWORD written{};
        const auto bytes = static_cast<DWORD>(rendered.size());

        // Seek and truncate directly; a failed short write remains eligible for the next collected snapshot.
        const bool succeeded = SetFilePointerEx(file, offset, nullptr, FILE_BEGIN) &&
                               WriteFile(file, rendered.data(), bytes, &written, nullptr) && written == bytes && SetEndOfFile(file);
        const auto error = succeeded ? ERROR_SUCCESS : (GetLastError() ? GetLastError() : ERROR_WRITE_FAULT);
        CloseHandle(file);
        if (!succeeded) {
            return {StatusPublishOutcome::Failed, "Could not write the FusionCutter.txt status report", error};
        }

        successful_ = std::string{content};
        return {StatusPublishOutcome::Written};
    } catch (...) {
        return {StatusPublishOutcome::Failed, "Could not update the FusionCutter.txt status report", ERROR_NOT_ENOUGH_MEMORY};
    }
}

} // namespace fc::reporting
