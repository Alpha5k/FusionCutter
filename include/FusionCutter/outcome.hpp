#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace fusioncutter {

using PatchId = std::string_view;

enum class InitializationOutcome {
    Completed,
    Unsupported,
    Fatal,
};

enum class PatchOutcome {
    Disabled,
    NotApplicable,
    WaitingForImage,
    Installed,
    Skipped,
    Failed,
};

struct OutcomeReason {
    std::string message;
    std::optional<std::string> operation;
    std::optional<PatchId> related_patch;
};

struct InitializationResult {
    InitializationOutcome outcome;
    std::optional<OutcomeReason> reason;
};

struct PatchResult {
    PatchId patch_id;
    std::string_view name;
    PatchOutcome outcome;
    std::optional<OutcomeReason> reason;
};

} // namespace fusioncutter
