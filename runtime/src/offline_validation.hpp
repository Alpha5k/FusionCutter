#pragma once

#include "patches/patch_record.hpp"
#include "native/plan.hpp"

#include <fc/base.hpp>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fc::validation {

// Supplies a game executable or DLL for recognition and plan validation.
struct ImageInput {
    std::filesystem::path path;
    // An empty profile accepts the recognized build; an explicit profile asserts its ID after PE recognition.
    std::string profile;
};

// Selects the files and startup settings used by an offline validation run.
struct ValidationInput {
    FC_Role role = FC_ROLE_CLIENT;
    std::vector<std::filesystem::path> plugins;
    std::vector<ImageInput> images;
    // Reads runtime and plugin INI files from this directory; omission or an empty path selects defaults.
    // Plugin settings can also come from environment overrides. Validation preserves all configuration files.
    std::optional<std::filesystem::path> config_directory;
};

// Keeps recognized PE metadata alongside the bytes arranged at their section RVAs.
struct ValidationImage {
    runtime::FileImage image;
    std::vector<std::byte> mapped;
};

// Owns recognized images, the handler host and selected patch records returned by offline validation.
struct ValidationResult {
    ValidationResult() = default;
    ValidationResult(ValidationResult &&) noexcept = default;
    ValidationResult &operator=(ValidationResult &&) = delete;

    // Plans borrow these mapped buffers; member order destroys the session and its plans before the buffers.
    std::vector<ValidationImage> images;
    // Captures runtime INI values and recoverable warnings for the validation report.
    runtime::RuntimeSettings runtime_settings;
    std::unique_ptr<FC_Host> host;
    std::unique_ptr<runtime::SelectionSession> session;
};

// Recognizes images, loads plugins and validates plans against mapped file bytes, stopping before Prepare or installation.
// Input failures return an error; plugin and patch failures remain in the returned session's diagnostics.
fc::Result<ValidationResult> validate_offline(const ValidationInput &input);

// Finds a patch by its ASCII case-insensitive Plugin.Patch ID, or returns null when it is absent.
// The result must come from successful validation; the returned patch belongs to its session.
const runtime::PatchRecord *find_patch(const ValidationResult &, std::string_view id) noexcept;

} // namespace fc::validation
