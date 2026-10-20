#pragma once

#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace fc::verify {

// Distinguishes completed checks from failures and checks unavailable offline.
enum class Status {
    Passed,
    Failed,
    NotChecked
};

// Identifies a submitted operation's image offset and author-supplied label.
struct Location {
    std::string profile;
    // Byte offset from the selected image's preferred base address.
    std::uint32_t rva{};
    std::string name;
};

// Holds one performed check or coverage gap for both output formats.
struct Check {
    Status status;
    std::string owner, operation, diagnostic;
    std::optional<Location> location;
    std::string evidence;
    std::uint32_t windows_error{};
};

// Retains the supplied path and recognized PE metadata for an image.
struct Image {
    std::string path, profile, filename;
    std::uint32_t size{};
    std::uint64_t preferred_base{};
};

// Connects a selected variant's image label to its recognized profile.
struct Binding {
    std::string label, profile;
};

// Retains patch states and selected variant identities even when planning is skipped.
struct Patch {
    std::string id, state, reason;
    bool enabled{};
    // Zero-based position in the patch's declared variants, absent when selection failed or is waiting.
    std::optional<std::size_t> variant;
    std::vector<Binding> images;
};

// Owns the validation results shared by readable and JSON output.
struct Report {
    std::string role;
    int exit_code{};
    std::vector<Image> images;
    std::vector<Patch> patches;
    std::vector<Check> checks;
};

// Returns the stable status spelling used in readable and JSON reports.
inline std::string_view status_name(Status status, bool human = false) {
    switch (status) {
    case Status::Passed:
        return human ? "Passed" : "passed";
    case Status::Failed:
        return human ? "Failed" : "failed";
    case Status::NotChecked:
        return human ? "Not checked" : "not_checked";
    }
    return "failed";
}

// Writes one UTF-8 string as JSON, escaping every control byte without changing non-ASCII text.
inline void json_string(std::ostream &out, std::string_view value) {
    constexpr char digits[] = "0123456789abcdef";
    out << '"';
    for (const unsigned char byte : value) {
        if (byte == '"' || byte == '\\') {
            out << '\\' << static_cast<char>(byte);
        } else if (byte < 0x20) {
            out << "\\u00" << digits[byte >> 4] << digits[byte & 15];
        } else {
            out << static_cast<char>(byte);
        }
    }
    out << '"';
}

// Writes the report as one JSON object; the caller checks the output stream for write failures.
inline void write_json(std::ostream &out, const Report &report) {
    out << "{\n  \"role\": ";
    json_string(out, report.role);
    out << ",\n  \"exit_code\": " << report.exit_code << ",\n  \"images\": [";
    bool first = true;
    for (const auto &image : report.images) {
        if (!first) {
            out << ',';
        }
        first = false;
        out << "\n    {\"path\": ";
        json_string(out, image.path);
        out << ", \"profile\": ";
        json_string(out, image.profile);
        out << ", \"filename\": ";
        json_string(out, image.filename);
        out << ", \"image_size\": " << image.size << ", \"preferred_base\": " << image.preferred_base << '}';
    }
    out << "\n  ],\n  \"patches\": [";
    first = true;
    for (const auto &patch : report.patches) {
        if (!first) {
            out << ',';
        }
        first = false;
        out << "\n    {\"id\": ";
        json_string(out, patch.id);
        out << ", \"state\": ";
        json_string(out, patch.state);
        out << ", \"enabled\": " << (patch.enabled ? "true" : "false") << ", \"reason\": ";
        json_string(out, patch.reason);
        out << ", \"variant\": ";
        if (patch.variant) {
            out << *patch.variant;
        } else {
            out << "null";
        }
        out << ", \"images\": [";
        bool first_binding = true;
        for (const auto &image : patch.images) {
            if (!first_binding) {
                out << ',';
            }
            first_binding = false;
            out << "{\"label\": ";
            json_string(out, image.label);
            out << ", \"profile\": ";
            json_string(out, image.profile);
            out << '}';
        }
        out << "]}";
    }
    out << "\n  ],\n  \"checks\": [";
    first = true;
    for (const auto &check : report.checks) {
        if (!first) {
            out << ',';
        }
        first = false;
        out << "\n    {\"status\": ";
        json_string(out, status_name(check.status));
        out << ", \"owner\": ";
        json_string(out, check.owner);
        out << ", \"operation\": ";
        json_string(out, check.operation);
        out << ", \"diagnostic\": ";
        json_string(out, check.diagnostic);
        out << ", \"windows_error\": " << check.windows_error << ", \"evidence\": ";
        json_string(out, check.evidence);
        out << ", \"location\": ";
        if (check.location) {
            out << "{\"profile\": ";
            json_string(out, check.location->profile);
            out << ", \"rva\": " << check.location->rva << ", \"name\": ";
            json_string(out, check.location->name);
            out << '}';
        } else {
            out << "null";
        }
        out << '}';
    }
    out << "\n  ]\n}\n";
}

// Prints selection identities and the same checks serialized by write_json, including every coverage gap.
inline void write_human(std::ostream &out, const Report &report) {
    out << "Offline verification (" << report.role << "):\n";
    for (const auto &image : report.images) {
        out << "  Image: " << image.profile << " = " << image.path << " (SizeOfImage " << image.size << ")\n";
    }
    for (const auto &patch : report.patches) {
        out << "  Patch state: " << patch.id << ": " << patch.state;
        if (patch.variant) {
            out << ", variant " << *patch.variant;
        }
        out << '\n';
        for (const auto &image : patch.images) {
            out << "    " << image.label << " = " << image.profile << '\n';
        }
    }
    for (const auto &check : report.checks) {
        out << "  " << status_name(check.status, true) << ": " << check.owner << " / " << check.operation;
        if (check.location) {
            out << " / " << check.location->profile << "+0x" << std::hex << check.location->rva << std::dec;
            if (!check.location->name.empty()) {
                out << " (" << check.location->name << ')';
            }
        }
        if (!check.evidence.empty()) {
            out << " / evidence " << check.evidence;
        }
        if (!check.diagnostic.empty()) {
            out << ": " << check.diagnostic;
        }
        if (check.windows_error) {
            out << " (Windows error " << check.windows_error << ')';
        }
        out << '\n';
    }
    out << (report.exit_code == 0   ? "All performed offline checks passed. Items marked Not checked remain untested.\n"
            : report.exit_code == 1 ? "Verification failed.\n"
                                    : "Verifier input or tool failed.\n");
}

} // namespace fc::verify
