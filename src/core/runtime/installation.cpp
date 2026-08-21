#include "installation.hpp"

#include "../fatal_boundary.hpp"

#include "../catalog/callback_error.hpp"
#include "../catalog/definition_copy.hpp"
#include "../patching/patch_preparation.hpp"
#include "../planning/resolution.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace fc::runtime {
namespace {

// One context routes the ABI's three capabilities from the Prepare phase to their process-lifetime owners.
struct PrepareBridge {
    InstallationAttempt* attempt{};
    const InstallationServices* services{};
    InterfaceRouter* interfaces{};
    catalog::PatchIndex patch{};
    // A rejected data resolution poisons the complete Prepare invocation, even for hand-written native callbacks.
    bool data_resolution_failed{};
};

// Resolves only storage owned by the current attempt, preventing a plugin from addressing another patch's allocation.
FC_Bool FC_CALL resolve_data(void* context, FC_DataHandle handle, std::uintptr_t* address,
                             std::uint64_t* byte_size) noexcept {
    // Outputs are invalid until the complete request succeeds, matching every other fallible ABI capability.
    if (address != nullptr) {
        *address = 0;
    }
    if (byte_size != nullptr) {
        *byte_size = 0;
    }
    if (context == nullptr) {
        return FC_FALSE;
    }
    auto& bridge = *static_cast<PrepareBridge*>(context);
    if (bridge.data_resolution_failed || address == nullptr || byte_size == nullptr ||
        handle == FC_INVALID_DATA_HANDLE || !bridge.attempt->transaction().resolve_data(handle, *address, *byte_size)) {
        bridge.data_resolution_failed = true;
        return FC_FALSE;
    }
    return FC_TRUE;
}

// Contains the optional trace service behind the non-throwing Prepare ABI and clears rejected output.
FC_TraceCreateResult FC_CALL create_trace(void* context, const FC_TraceDefinition* definition,
                                          FC_TraceHandle* output) noexcept {
    if (output != nullptr) {
        *output = nullptr;
    }
    if (context == nullptr) {
        return FC_TRACE_REJECTED;
    }
    auto& bridge = *static_cast<PrepareBridge*>(context);
    if (bridge.services->traces == nullptr) {
        return FC_TRACE_REJECTED;
    }
    return bridge.attempt->create_trace(*bridge.services->traces, bridge.patch, definition, output);
}

// Exposes only interfaces already published by installed providers through the runtime's sole routing authority.
FC_Bool FC_CALL find_interface(void* context, FC_StringView provider_patch, FC_StringView id, std::uint32_t size,
                               void* output) noexcept {
    if (context == nullptr) {
        return FC_FALSE;
    }
    const auto& bridge = *static_cast<PrepareBridge*>(context);
    if (bridge.interfaces == nullptr || (provider_patch.data == nullptr && provider_patch.size != 0) ||
        (id.data == nullptr && id.size != 0)) {
        return FC_FALSE;
    }
    const auto provider =
        provider_patch.size == 0 ? std::string_view{} : std::string_view{provider_patch.data, provider_patch.size};
    const auto interface_id = id.size == 0 ? std::string_view{} : std::string_view{id.data, id.size};
    return bridge.interfaces->find_active(provider, interface_id, size, output);
}

[[nodiscard]] const catalog::SupportDefinitionRecord& selected_support(const PatchRuntimeState& runtime,
                                                                       catalog::PatchIndex patch) noexcept {
    const auto& definition = runtime.catalog.patch(patch);
    return definition.supports[*definition.selected_support];
}

[[nodiscard]] FC_FailurePolicy effective_policy(const PatchRuntimeState& runtime, catalog::PatchIndex patch) noexcept {
    const auto& definition = runtime.catalog.patch(patch);
    const auto support_policy = selected_support(runtime, patch).failure_policy;
    return support_policy == FC_FAILURE_INHERIT ? definition.failure_policy : support_policy;
}

[[nodiscard]] bool terminal_failure(planning::PatchState state) noexcept {
    return state == planning::PatchState::Failed || state == planning::PatchState::Skipped;
}

// Finds the first terminal failure whose effective policy requires startup to stop globally.
[[nodiscard]] std::optional<catalog::PatchIndex> fatal_patch(const PatchRuntimeState& runtime) noexcept {
    for (const auto& record : runtime.patches.records()) {
        if (terminal_failure(record.state) && effective_policy(runtime, record.patch) == FC_FAILURE_FATAL) {
            return record.patch;
        }
    }
    return std::nullopt;
}

// Converts every remaining viable patch to Skipped while retaining the original fatal patch as diagnostic cause.
void finalize_fatal(PatchRuntimeState& runtime, catalog::PatchIndex cause) {
    const auto& cause_id = runtime.catalog.patch(cause).id;
    const auto& cause_record = runtime.patches.record(cause);
    const auto phase = cause_record.reason && cause_record.reason->phase ? *cause_record.reason->phase
                                                                         : planning::PatchPhase::Selection;
    for (auto& record : runtime.patches.records()) {
        if (record.state != planning::PatchState::Pending && record.state != planning::PatchState::Ready &&
            record.state != planning::PatchState::WaitingForImage) {
            continue;
        }
        planning::finish_inactive_patch(record, planning::PatchState::Skipped,
                                        {.message = "Startup stopped after a fatal patch result",
                                         .phase = phase,
                                         .operation = "Finalize fatal installation",
                                         .related_patch = cause_id});
    }
}

[[nodiscard]] bool claims_conflict(const planning::MemoryClaim& left, const planning::MemoryClaim& right) noexcept {
    if (left.image != right.image) {
        return false;
    }
    const auto left_end = static_cast<std::uint64_t>(left.rva) + left.size;
    const auto right_end = static_cast<std::uint64_t>(right.rva) + right.size;
    const bool overlap = left.rva < right_end && right.rva < left_end;
    return overlap && (left.access == planning::ClaimAccess::Write || right.access == planning::ClaimAccess::Write);
}

// Selects a deterministic blocker when exposed failures overlap the candidate's claims from its patch plan.
[[nodiscard]] const planning::MemoryClaim* blocked_claim(const PatchRuntimeState& runtime,
                                                         const planning::PatchWorkRecord& patch) {
    const planning::MemoryClaim* selected{};
    for (const auto& candidate : patch.plan.claims) {
        for (const auto& blocker : runtime.blocked_claims) {
            if (!claims_conflict(candidate, blocker)) {
                continue;
            }
            if (selected == nullptr || catalog::fold_ascii(runtime.catalog.patch(blocker.patch).id) <
                                           catalog::fold_ascii(runtime.catalog.patch(selected->patch).id)) {
                selected = &blocker;
            }
        }
    }
    // Hook write claims are materialized only by the participant that actually creates an absent site. The
    // prerequisite guard must still compare every accepted participant with a retained failed creator at that site.
    const auto image = selected_support(runtime, patch.patch).image;
    for (const auto& operation : patch.plan.operations) {
        const auto* hook = std::get_if<planning::HookOperation>(&operation.payload);
        if (hook == nullptr) {
            continue;
        }
        const planning::MemoryClaim candidate{.patch = patch.patch,
                                              .image = image,
                                              .rva = hook->location.rva,
                                              .size = hook->overwrite_size,
                                              .access = planning::ClaimAccess::Write,
                                              .operation_index = operation.index};
        for (const auto& blocker : runtime.blocked_claims) {
            if (claims_conflict(candidate, blocker) &&
                (selected == nullptr || catalog::fold_ascii(runtime.catalog.patch(blocker.patch).id) <
                                            catalog::fold_ascii(runtime.catalog.patch(selected->patch).id))) {
                selected = &blocker;
            }
        }
    }
    return selected;
}

// Selects the first dependency that failed to reach the Installed state before its consumer's turn.
[[nodiscard]] const planning::RequiredEdge* unavailable_requirement(const PatchRuntimeState& runtime,
                                                                    const planning::PatchWorkRecord& patch) {
    const planning::RequiredEdge* unavailable{};
    for (const auto& edge : patch.required_edges) {
        if (runtime.patches.record(edge.provider).state == planning::PatchState::Installed) {
            continue;
        }
        if (unavailable == nullptr) {
            unavailable = &edge;
            continue;
        }
        const auto source = catalog::fold_ascii(edge.declared_source);
        const auto selected_source = catalog::fold_ascii(unavailable->declared_source);
        const auto provider_id = catalog::fold_ascii(runtime.catalog.patch(edge.provider).id);
        const auto selected_id = catalog::fold_ascii(runtime.catalog.patch(unavailable->provider).id);
        if (source < selected_source || (source == selected_source && provider_id < selected_id)) {
            unavailable = &edge;
        }
    }
    return unavailable;
}

// Terminalizes a consumer whose provider did not install, then propagates that unavailability through dependents.
void skip_prerequisite(PatchRuntimeState& runtime, planning::PatchWorkRecord& patch,
                       const planning::RequiredEdge& edge) {
    const auto& provider = runtime.patches.record(edge.provider);
    const auto phase =
        provider.reason && provider.reason->phase ? *provider.reason->phase : planning::PatchPhase::Prepare;
    const auto group = runtime.catalog.find_group(edge.declared_source);
    planning::finish_inactive_patch(
        patch, planning::PatchState::Skipped,
        {.message = "Required patch did not reach the Installed state",
         .phase = phase,
         .operation = "Guard installation prerequisite",
         .related_patch = runtime.catalog.patch(edge.provider).id,
         .related_group = group ? std::optional<std::string_view>{runtime.catalog.group(*group).id} : std::nullopt});
    planning::prune_unavailable_consumers(runtime.patches);
}

// Prevents later patches from touching native state retained after a commit exposure or failed rollback.
void skip_blocked_claim(PatchRuntimeState& runtime, planning::PatchWorkRecord& patch,
                        const planning::MemoryClaim& blocker) {
    planning::finish_inactive_patch(patch, planning::PatchState::Skipped,
                                    {.message = "Retained exposed failure blocks a required native range",
                                     .phase = planning::PatchPhase::Prepare,
                                     .operation = "Guard retained native claims",
                                     .related_patch = runtime.catalog.patch(blocker.patch).id});
    planning::prune_unavailable_consumers(runtime.patches);
}

// Invokes the plugin's optional Prepare callback with attempt-scoped storage and process-lifetime services.
[[nodiscard]] std::expected<void, planning::FailureReason>
invoke_prepare(const InstallationServices& services, InterfaceRouter& interfaces, planning::PatchInstance& instance,
               InstallationAttempt& attempt, catalog::PatchIndex patch) {
    const auto& callbacks = instance.callbacks();
    if (callbacks.prepare == nullptr) {
        return {};
    }
    PrepareBridge bridge{&attempt, &services, &interfaces, patch};
    const FC_PrepareContext context{.struct_size = sizeof(FC_PrepareContext),
                                    .report = services.prepare_report,
                                    .context = &bridge,
                                    .resolve_data = &resolve_data,
                                    .create_trace = &create_trace,
                                    .find_interface = &find_interface};
    catalog::CallbackError callback_error;
    const auto error = callback_error.sink();
    FC_CallStatus status = FC_CALL_FAILED;
    // The Prepare phase is recoverable while no native replacement bytes have been committed.
    try {
        status = callbacks.prepare(callbacks.context, instance.get(), &context, &error);
    } catch (...) {
        return std::unexpected(
            planning::FailureReason{.message = "The patch's Prepare callback threw across the native boundary",
                                    .phase = planning::PatchPhase::Prepare,
                                    .operation = "Prepare patch"});
    }
    // Capability failure takes precedence over a callback that ignored FC_FALSE and reported success afterward.
    if (bridge.data_resolution_failed) {
        return std::unexpected(
            planning::FailureReason{.message = "The Prepare phase could not resolve planned native data",
                                    .phase = planning::PatchPhase::Prepare,
                                    .operation = "Resolve native data"});
    }
    if (status != FC_CALL_OK) {
        return std::unexpected(planning::FailureReason{
            .message = callback_error.supplied && !callback_error.message.empty() ? callback_error.message
                                                                                  : "Patch's Prepare callback failed",
            .phase = planning::PatchPhase::Prepare,
            .operation = callback_error.operation.empty() ? "Prepare patch" : callback_error.operation});
    }
    return {};
}

// Runs the Activate callback after the Commit phase; its ABI cannot fail because native state is permanent.
void invoke_activate(const InstallationServices& services, planning::PatchInstance& instance) noexcept {
    const auto& callbacks = instance.callbacks();
    if (callbacks.activate == nullptr) {
        return;
    }
    const FC_ActivateContext context{.struct_size = sizeof(FC_ActivateContext), .report = services.activate_report};
    try {
        callbacks.activate(callbacks.context, instance.get(), &context);
    } catch (...) {
        // The Activate callback is non-failing after the Commit phase; unwinding cannot enter a recoverable path.
        fatal_invariant("An installed patch's Activate callback threw across the native boundary");
    }
}

// Releases plugin and attempt resources while no replacement byte has escaped, leaving a normal Failed record.
void fail_before_exposure(PatchRuntimeState& runtime, planning::PatchWorkRecord& patch, InstallationAttempt* attempt,
                          planning::FailureReason reason) {
    // Unbind Original handles before plugin-owned slots die, then clean up while prepared storage remains alive.
    if (attempt != nullptr) {
        attempt->clear_original_bindings();
    }
    patch.instance = {};
    if (attempt != nullptr) {
        attempt->release_prepared_resources();
    }
    patch.settings = {};
    patch.plan = {};
    patch.state = planning::PatchState::Failed;
    patch.reason = std::move(reason);
    planning::prune_unavailable_consumers(runtime.patches);
}

// Preserves all owners needed by possibly modified native state and converts their claims into future blockers.
void retain_exposed_failure(PatchRuntimeState& runtime, planning::PatchWorkRecord& patch, InstallationAttempt attempt,
                            patching::CommitFailure failure, CrashReporter& crash) {
    const auto begin = runtime.blocked_claims.size();
    // Exposure promotes even read requirements to exclusive blockers; later work cannot assume the retained state.
    for (auto claim : attempt.claims()) {
        claim.access = planning::ClaimAccess::Write;
        runtime.blocked_claims.push_back(claim);
    }
    const auto count = runtime.blocked_claims.size() - begin;
    runtime.retained_failures.push_back(
        {patch.patch, failure.rollback, std::move(attempt), std::move(patch.instance), begin, count});
    // Publish only after the retained record becomes the process-lifetime authority for every referenced resource.
    const auto& retained = runtime.retained_failures.back();
    crash.publish_retained_failure(patch.patch, failure.rollback,
                                   std::span{runtime.blocked_claims}.subspan(begin, count),
                                   runtime.catalog.patch(patch.patch).id, runtime.target,
                                   retained.attempt.transaction().resources(), retained.attempt.hook_preparation());
    patch.settings = {};
    patch.plan = {};
    patch.state = planning::PatchState::Failed;
    patch.reason = std::move(failure.reason);
    planning::prune_unavailable_consumers(runtime.patches);
}

// Computes the worst-case permanent claim count so installation never grows a vector after native exposure.
[[nodiscard]] std::optional<std::size_t> total_claim_capacity(const PatchRuntimeState& runtime) noexcept {
    if (runtime.installed_claims.size() > std::numeric_limits<std::size_t>::max() - runtime.blocked_claims.size()) {
        return std::nullopt;
    }
    std::size_t result = runtime.installed_claims.size() + runtime.blocked_claims.size();
    for (const auto& record : runtime.patches.records()) {
        if (record.state != planning::PatchState::Ready) {
            continue;
        }
        if (record.plan.claims.size() > std::numeric_limits<std::size_t>::max() - result) {
            return std::nullopt;
        }
        result += record.plan.claims.size();
        // Every hook may need one physical write claim if earlier creators at its site fail before exposure.
        const auto hook_count =
            static_cast<std::size_t>(std::ranges::count_if(record.plan.operations, [](const auto& operation) {
                return std::holds_alternative<planning::HookOperation>(operation.payload);
            }));
        if (hook_count > std::numeric_limits<std::size_t>::max() - result) {
            return std::nullopt;
        }
        result += hook_count;
    }
    return result;
}

// Counts the maximum installed sites; shared locations only make this reservation more conservative.
[[nodiscard]] std::optional<std::size_t> total_hook_capacity(const PatchRuntimeState& runtime) noexcept {
    auto result = runtime.hooks.installed_sites().size();
    for (const auto& record : runtime.patches.records()) {
        if (record.state != planning::PatchState::Ready) {
            continue;
        }
        const auto count =
            static_cast<std::size_t>(std::ranges::count_if(record.plan.operations, [](const auto& operation) {
                return std::holds_alternative<planning::HookOperation>(operation.payload);
            }));
        if (count > std::numeric_limits<std::size_t>::max() - result) {
            return std::nullopt;
        }
        result += count;
    }
    return result;
}

// Orders public IDs without folded copies while publishing the list of Update callbacks after the Commit phase.
[[nodiscard]] bool ascii_case_insensitive_less(std::string_view left, std::string_view right) noexcept {
    const auto shared = std::min(left.size(), right.size());
    for (std::size_t index = 0; index < shared; ++index) {
        const auto left_byte = static_cast<unsigned char>(left[index]);
        const auto right_byte = static_cast<unsigned char>(right[index]);
        const auto left_folded = left_byte >= 'A' && left_byte <= 'Z' ? left_byte + ('a' - 'A') : left_byte;
        const auto right_folded = right_byte >= 'A' && right_byte <= 'Z' ? right_byte + ('a' - 'A') : right_byte;
        if (left_folded != right_folded) {
            return left_folded < right_folded;
        }
    }
    return left.size() < right.size();
}

// Reserves known bindings before the Commit phase; plans for later images repeat the calculation for new work.
[[nodiscard]] std::optional<std::size_t> total_interface_capacity(const PatchRuntimeState& runtime) noexcept {
    auto result = runtime.interfaces.binding_count();
    for (const auto& record : runtime.patches.records()) {
        if (record.state != planning::PatchState::Ready) {
            continue;
        }
        const auto count = static_cast<std::size_t>(std::ranges::count_if(record.plan.operations, [](const auto& op) {
            return std::holds_alternative<planning::InterfaceBindingOperation>(op.payload);
        }));
        if (count > std::numeric_limits<std::size_t>::max() - result) {
            return std::nullopt;
        }
        result += count;
    }
    return result;
}

void mark_framework_fatal(PatchRuntimeState& runtime, std::string message) {
    // Give the integrity failure one patch authority, then use the ordinary fatal finalization for every survivor.
    for (auto& record : runtime.patches.records()) {
        if (record.state != planning::PatchState::Pending && record.state != planning::PatchState::Ready &&
            record.state != planning::PatchState::WaitingForImage) {
            continue;
        }
        const auto cause = record.patch;
        planning::finish_inactive_patch(record, planning::PatchState::Failed,
                                        {.message = std::move(message),
                                         .phase = planning::PatchPhase::Prepare,
                                         .operation = "Reserve installation records"});
        finalize_fatal(runtime, cause);
        return;
    }
}

} // namespace

InstallationResult install_ready_patches(const planning::InstallationPlan& plan, PatchRuntimeState& runtime,
                                         TraceSession& traces, CrashReporter& crash, InstallationServices services) {
    services.traces = &traces;
    services.logger.info("Installing {} ready patch(es)", plan.installation_order.size());
    struct CurrentPatchGuard {
        CrashReporter& crash;
        ~CurrentPatchGuard() {
            crash.clear_current_patch();
        }
    } current_patch_guard{crash};

    if (auto fatal = fatal_patch(runtime)) {
        services.logger.error("Cannot begin installation because patch '{}' already has a fatal result",
                              runtime.catalog.patch(*fatal).id);
        finalize_fatal(runtime, *fatal);
        return InstallationResult::Fatal;
    }

    // Reserve every publication owner before the first Commit phase so no fallible container growth remains afterward.
    const auto claim_capacity = total_claim_capacity(runtime);
    const auto hook_capacity = total_hook_capacity(runtime);
    const auto interface_capacity = total_interface_capacity(runtime);
    if (!claim_capacity || !hook_capacity || !interface_capacity) {
        services.logger.error("Publication capacity overflowed before native exposure");
        mark_framework_fatal(runtime, "Installation publication capacity overflowed");
        return InstallationResult::Fatal;
    }
    try {
        runtime.installed_patches.reserve(runtime.catalog.patch_count());
        runtime.retained_failures.reserve(runtime.catalog.patch_count());
        runtime.update_order.reserve(runtime.catalog.patch_count());
        runtime.installed_claims.reserve(*claim_capacity);
        runtime.blocked_claims.reserve(*claim_capacity);
        runtime.hooks.reserve(*hook_capacity);
        runtime.interfaces.reserve(runtime.catalog.patch_count(), *interface_capacity);
    } catch (...) {
        services.logger.error("Could not reserve final capacities for installation publication");
        mark_framework_fatal(runtime, "Installation publication capacity could not be reserved");
        return InstallationResult::Fatal;
    }

    for (const auto patch_index : plan.installation_order) {
        auto& patch = runtime.patches.record(patch_index);
        if (patch.state != planning::PatchState::Ready) {
            continue;
        }

        const auto& patch_id = runtime.catalog.patch(patch_index).id;
        services.logger.debug("Preparing patch '{}'", patch_id);

        crash.set_current_patch(patch_index, planning::PatchPhase::Prepare);
        // Every lifecycle context carries the token used by the Create, Plan, and Update callbacks and live status.
        auto patch_services = services;
        patch_services.prepare_report = catalog::report_token(patch_index);
        patch_services.activate_report = catalog::report_token(patch_index);

        // Prerequisite guard: frozen dependencies and newly retained blockers may only remove later work.
        if (const auto* edge = unavailable_requirement(runtime, patch)) {
            skip_prerequisite(runtime, patch, *edge);
        } else if (const auto* blocker = blocked_claim(runtime, patch)) {
            skip_blocked_claim(runtime, patch, *blocker);
        }
        if (patch.state != planning::PatchState::Ready) {
            if (patch.reason) {
                services.logger.warning("Patch '{}' was skipped before the Prepare phase: {}", patch_id,
                                        patch.reason->message);
            }
            if (auto fatal = fatal_patch(runtime)) {
                finalize_fatal(runtime, *fatal);
                return InstallationResult::Fatal;
            }
            crash.clear_current_patch();
            continue;
        }

        // During the Prepare phase, allocate, resolve, revalidate inputs visible to the callback, invoke the Prepare
        // callback, revalidate all inputs, then allocate snapshots made permanent by the Commit phase and activation.
        auto transaction = patching::prepare_patch_transaction(
            runtime.target, runtime.catalog, patch,
            services.memory_writer.write == nullptr ? patching::system_memory_writer() : services.memory_writer);
        if (!transaction) {
            fail_before_exposure(runtime, patch, nullptr, std::move(transaction.error()));
        } else {
            // Claims move into the attempt before the Commit phase, preventing allocation after native exposure.
            InstallationAttempt attempt{std::move(*transaction), std::move(patch.plan.claims)};
            auto hook_preparation = runtime.hooks.prepare_patch(runtime.target, runtime.catalog, patch,
                                                                attempt.transaction(), attempt.mutable_claims());
            std::expected<void, planning::FailureReason> prepared;
            if (!hook_preparation) {
                prepared = std::unexpected(std::move(hook_preparation.error()));
            } else {
                attempt.set_hook_preparation(std::move(*hook_preparation));
                prepared = patching::revalidate_prepare_inputs(runtime.target, runtime.catalog, patch,
                                                               runtime.hooks.installed_sites());
            }
            if (prepared) {
                prepared = attempt.bind_originals(runtime.hooks);
            }
            if (prepared) {
                prepared = invoke_prepare(patch_services, runtime.interfaces, patch.instance, attempt, patch_index);
            }
            if (prepared) {
                prepared = patching::revalidate_commit_inputs(runtime.target, runtime.catalog, patch,
                                                              runtime.hooks.installed_sites());
            }
            if (prepared) {
                prepared = attempt.finalize_interfaces(runtime.interfaces, patch.plan);
            }
            if (prepared) {
                prepared = attempt.finalize_hooks(runtime.hooks, runtime.catalog);
            }
            if (!prepared) {
                fail_before_exposure(runtime, patch, &attempt, std::move(prepared.error()));
            } else {
                // The Commit phase applies the transaction, which owns its complete reverse unwind on failure.
                services.logger.debug("Committing native changes for patch '{}'", patch_id);
                crash.set_patch_phase(planning::PatchPhase::Commit);
                auto committed = attempt.transaction().commit();
                if (!committed) {
                    auto failure = std::move(committed.error());
                    if (failure.rollback == patching::RollbackResult::NoExposure) {
                        const bool contained = failure.contained;
                        fail_before_exposure(runtime, patch, &attempt, std::move(failure.reason));
                        if (!contained) {
                            services.logger.error(
                                "Patch '{}' failed during the Commit phase without a contained rollback", patch_id);
                            finalize_fatal(runtime, patch_index);
                            return InstallationResult::Fatal;
                        }
                    } else {
                        const bool contained = failure.contained;
                        retain_exposed_failure(runtime, patch, std::move(attempt), std::move(failure), crash);
                        if (!contained) {
                            services.logger.error("Patch '{}' left uncontained native state after the Commit phase",
                                                  patch_id);
                            finalize_fatal(runtime, patch_index);
                            return InstallationResult::Fatal;
                        }
                    }
                } else {
                    // Non-failing activation and publication: all permanent vectors already have prepared capacity.
                    services.logger.debug("Activating and publishing patch '{}'", patch_id);
                    crash.set_patch_phase(planning::PatchPhase::Activate);
                    // Trace producers become usable before the Activate callback can start a private worker.
                    attempt.arm_traces(traces);
                    invoke_activate(patch_services, patch.instance);
                    // Arm this consumer first, then expose its provider contract to waiting installed consumers.
                    attempt.publish_interfaces(runtime.interfaces, patch_index);
                    runtime.interfaces.publish_provider(patch_index, patch.instance,
                                                        runtime.catalog.patch(patch_index).id);
                    // Hook snapshots open only after every eligible interface connection can observe armed patch state.
                    attempt.publish_hooks(runtime.hooks);
                    // Framework resources and the handler enter vectors whose capacity predates the Commit phase.
                    runtime.installed_claims.insert(runtime.installed_claims.end(), attempt.claims().begin(),
                                                    attempt.claims().end());
                    runtime.installed_patches.push_back(
                        {patch_index, std::move(*committed), std::move(patch.instance)});
                    patch.reason.reset();
                    patch.state = planning::PatchState::Installed;
                    // Direct lookup waits for full hook and ownership publication, unlike the earlier internal route.
                    runtime.interfaces.mark_active(patch_index);
                    // Only present Update callbacks enter the retained case-insensitive dispatch list.
                    if (runtime.installed_patches.back().instance.has_update()) {
                        const auto installed_index =
                            static_cast<InstalledRecordIndex>(runtime.installed_patches.size() - 1);
                        const auto position =
                            std::lower_bound(runtime.update_order.begin(), runtime.update_order.end(), installed_index,
                                             [&](InstalledRecordIndex left, InstalledRecordIndex right) {
                                                 return ascii_case_insensitive_less(
                                                     runtime.catalog.patch(runtime.installed_patches[left].patch).id,
                                                     runtime.catalog.patch(runtime.installed_patches[right].patch).id);
                                             });
                        runtime.update_order.insert(position, installed_index);
                    }
                    // Crash capture sees installed identity and native owners before temporary plan data is cleared.
                    crash.publish_installed_patch(patch_index, runtime.catalog, runtime.target, patch.plan,
                                                  runtime.installed_patches.back().resources, runtime.hooks);
                    patch.settings = {};
                    patch.plan = {};
                    services.logger.info("Installed patch '{}'", patch_id);
                }
            }
        }

        // Every local failure becomes terminal before logging, so the message reflects its final ownership path.
        if (patch.state == planning::PatchState::Failed && patch.reason) {
            services.logger.error("Patch '{}' failed in '{}': {}", patch_id,
                                  patch.reason->operation.value_or("Install patch"), patch.reason->message);
        } else if (patch.state == planning::PatchState::Skipped && patch.reason) {
            services.logger.warning("Patch '{}' was skipped during installation: {}", patch_id, patch.reason->message);
        }

        if (auto fatal = fatal_patch(runtime)) {
            finalize_fatal(runtime, *fatal);
            return InstallationResult::Fatal;
        }
        crash.clear_current_patch();
    }
    services.logger.info("Installed {} patch(es); retained {} failed attempt(s)", runtime.installed_patches.size(),
                         runtime.retained_failures.size());
    return InstallationResult::Completed;
}

InstallationResult install_ready_patches(const planning::InstallationPlan& plan, PatchRuntimeState& runtime,
                                         InstallationServices services) {
    // The convenience entry keeps component tests on the production installer while supplying inert local reporters.
    TraceSession traces{0};
    CrashPhaseCursors cursors;
    CrashReporter crash{cursors};
    static_cast<void>(crash.install());
    return install_ready_patches(plan, runtime, traces, crash, services);
}

} // namespace fc::runtime
