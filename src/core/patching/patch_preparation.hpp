#pragma once

#include "patch_transaction.hpp"

#include "../planning/planning_types.hpp"
#include "../targets/recognition.hpp"

#include <expected>
#include <span>

namespace fc::patching {

// Allocates symbolic storage, resolves final encodings, and prepares non-hook mutations for one validated patch plan.
[[nodiscard]] std::expected<PatchTransaction, planning::FailureReason>
prepare_patch_transaction(const targets::RecognizedTarget& target, const catalog::Catalog& catalog,
                          const planning::PatchWorkRecord& patch, NativeMemoryWriter writer = system_memory_writer());

// This pass checks addresses and callables that the Prepare callback could have retained from the Plan callback.
[[nodiscard]] std::expected<void, planning::FailureReason>
revalidate_prepare_inputs(const targets::RecognizedTarget& target, const catalog::Catalog& catalog,
                          const planning::PatchWorkRecord& patch,
                          std::span<const planning::InstalledHookSite> installed_hooks = {});

// The final bounded pass rechecks author evidence and structural snapshots before the Commit phase.
[[nodiscard]] std::expected<void, planning::FailureReason>
revalidate_commit_inputs(const targets::RecognizedTarget& target, const catalog::Catalog& catalog,
                         const planning::PatchWorkRecord& patch,
                         std::span<const planning::InstalledHookSite> installed_hooks = {});

} // namespace fc::patching
