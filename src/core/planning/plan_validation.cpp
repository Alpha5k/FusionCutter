#include "plan_validation.hpp"

#include "evidence_validation.hpp"
#include "native_address.hpp"
#include "native_call.hpp"
#include "resolution.hpp"

#include "../catalog/callback_error.hpp"
#include "../catalog/definition_copy.hpp"

#include <Zydis/Zydis.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <limits>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fc::planning {
namespace {

inline constexpr std::size_t kInterfaceByteCapacity = 512;
inline constexpr std::size_t kHookDecodeByteCapacity = 64;
inline constexpr std::uint32_t kMinimumHookOverwrite = 5;

// Borrowed ABI primitive helpers reject malformed views and scalar forms before PlanCollector follows their pointers.
[[nodiscard]] bool power_of_two(std::uint64_t value) noexcept {
    return value != 0 && (value & (value - 1)) == 0;
}

[[nodiscard]] bool canonical_boolean(FC_Bool value) noexcept {
    return value == FC_FALSE || value == FC_TRUE;
}

[[nodiscard]] bool checked_extent(std::uint32_t rva, std::uint64_t size, std::size_t image_size) noexcept {
    return size != 0 && rva <= image_size && size <= image_size - rva;
}

[[nodiscard]] std::size_t pointer_size(FC_Architecture architecture) noexcept {
    return architecture == FC_ARCH_X86 ? 4U : 8U;
}

[[nodiscard]] std::string_view native_string_view(FC_StringView view) noexcept {
    return view.size == 0 ? std::string_view{} : std::string_view{view.data, view.size};
}

[[nodiscard]] std::span<const std::byte> native_byte_view(FC_ByteView view) noexcept {
    return view.size == 0 ? std::span<const std::byte>{}
                          : std::span{reinterpret_cast<const std::byte*>(view.data), view.size};
}

[[nodiscard]] bool valid_string_view(FC_StringView view) noexcept {
    return view.size == 0 || view.data != nullptr;
}

[[nodiscard]] bool valid_byte_view(FC_ByteView view) noexcept {
    return view.size == 0 || view.data != nullptr;
}

[[nodiscard]] bool empty_byte_view(FC_ByteView view) noexcept {
    // All zero-length ABI leaf views are empty; their pointer is deliberately ignored.
    return view.size == 0;
}

[[nodiscard]] bool empty_address_target(const FC_AddressTarget& target) noexcept {
    return target.kind == 0 && target.image_rva == 0 && target.data == FC_INVALID_DATA_HANDLE &&
           target.data_offset == 0 && target.plugin_function == 0;
}

[[nodiscard]] const catalog::SupportDefinitionRecord& selected_support(const catalog::Catalog& catalog,
                                                                       catalog::PatchIndex patch) noexcept {
    const auto& definition = catalog.patch(patch);
    return definition.supports[*definition.selected_support];
}

[[nodiscard]] FC_TargetInfo target_info(const targets::RecognizedTarget& target, FC_TargetImage image) noexcept {
    const auto info = target.info(image);
    return {.layout = info.layout,
            .role = info.role,
            .architecture = info.architecture,
            .image_profile = {info.image_profile.data(), static_cast<std::uint32_t>(info.image_profile.size())}};
}

// A polled module can already be executing, so only operations proven safe while the image is live survive.
[[nodiscard]] bool safe_for_late_module(const OperationRecord& record, targets::LateMutationPolicy policy) noexcept {
    if (policy != targets::LateMutationPolicy::SuspendedHooksOnly) {
        return false;
    }
    if (const auto* requirement = std::get_if<RequireOperation>(&record.payload)) {
        // A writable resolved pointer would let later plugin code bypass the suspended publication mechanism.
        return !requirement->writable;
    }
    if (const auto* hook = std::get_if<HookOperation>(&record.payload)) {
        // Function and instruction hooks publish through SafetyHook thread trapping; direct call sites are raw writes.
        return hook->kind == FC_HOOK_FUNCTION_ENTRY || hook->kind == FC_HOOK_INSTRUCTION;
    }
    // Allocations and copied interface routes do not touch the target image; every other payload rewrites it directly.
    return std::holds_alternative<DataAllocationOperation>(record.payload) ||
           std::holds_alternative<InterfaceBindingOperation>(record.payload);
}

// Applies the recognized profile's safety contract to one copied patch plan before conflict handling.
[[nodiscard]] std::expected<void, std::string> validate_late_mutation_policy(const targets::RecognizedTarget& target,
                                                                             FC_TargetImage image,
                                                                             const SubmittedPlan& plan) {
    const auto* profile = targets::find_image_profile(target.info(image).image_profile);
    if (profile == nullptr) {
        return std::unexpected("The selected image has no reviewed target profile");
    }
    if (!profile->late) {
        return {};
    }
    if (!std::ranges::all_of(plan.operations, [&](const OperationRecord& operation) {
            return safe_for_late_module(operation, profile->late->mutation);
        })) {
        return std::unexpected("Patch plan for a late image contains a mutation unsafe for a module already in use");
    }
    return {};
}

[[nodiscard]] bool claims_overlap(const MemoryClaim& left, const MemoryClaim& right) noexcept {
    if (left.image != right.image) {
        return false;
    }
    const auto left_end = static_cast<std::uint64_t>(left.rva) + left.size;
    const auto right_end = static_cast<std::uint64_t>(right.rva) + right.size;
    return left.rva < right_end && right.rva < left_end;
}

// Owns one callback-scoped sink and converts each accepted borrowed request into storage retained by the patch plan.
class PlanCollector {
  public:
    PlanCollector(const targets::RecognizedTarget& target, const catalog::Catalog& catalog, catalog::PatchIndex patch,
                  ValidationBaseline baseline) noexcept
        : target_(target), catalog_(catalog), patch_(patch), support_(selected_support(catalog, patch)),
          image_(*target.find(support_.image)), owner_(catalog.plugin(catalog.patch_plugin(patch)).code_owner),
          baseline_(baseline) {}

    [[nodiscard]] const FC_PlanSink& sink() const noexcept {
        return sink_;
    }

    [[nodiscard]] bool failed() const noexcept {
        return failure_.has_value();
    }

    [[nodiscard]] const std::string& failure() const noexcept {
        return *failure_;
    }

    [[nodiscard]] SubmittedPlan take_plan() && noexcept {
        return std::move(plan_);
    }

  private:
    // Rejection is sticky, preserving the first defect as the Plan callback's authoritative failure.
    [[nodiscard]] FC_SubmitResult reject(std::string message) noexcept {
        if (!failure_) {
            failure_ = std::move(message);
        }
        return FC_SUBMIT_REJECTED;
    }

    // Charges all retained strings, bytes, records, and call metadata against one per-patch ownership budget.
    [[nodiscard]] bool charge(std::size_t byte_size) noexcept {
        if (byte_size > kPatchPlanByteCapacity - plan_.retained_bytes) {
            (void)reject("Patch plan exceeds the copied payload capacity");
            return false;
        }
        plan_.retained_bytes += byte_size;
        return true;
    }

    // Copy helpers turn borrowed ABI views and tagged records into validated patch plan values owned by the framework.
    [[nodiscard]] std::expected<std::string, std::string> copy_string(FC_StringView view) {
        if (!valid_string_view(view) || !charge(view.size)) {
            return std::unexpected("Patch plan contains an invalid string view or exceeds its string budget");
        }
        const auto value = native_string_view(view);
        if (!catalog::valid_utf8(value)) {
            return std::unexpected("Patch plan text is not valid UTF-8");
        }
        return std::string{value};
    }

    [[nodiscard]] std::expected<std::vector<std::byte>, std::string> copy_bytes(FC_ByteView view) {
        if (!valid_byte_view(view) || !charge(view.size)) {
            return std::unexpected("Patch plan contains an invalid byte view or exceeds its byte budget");
        }
        const auto bytes = native_byte_view(view);
        return std::vector<std::byte>{bytes.begin(), bytes.end()};
    }

    [[nodiscard]] std::expected<EvidenceRecord, std::string> copy_evidence(const FC_Evidence& evidence) {
        EvidenceRecord result{.kind = evidence.kind, .target_rva = evidence.target_rva};
        if (evidence.kind == FC_EVIDENCE_NONE) {
            if (!empty_byte_view(evidence.bytes) || !empty_byte_view(evidence.mask) || evidence.target_rva != 0) {
                return std::unexpected("Empty evidence contains an active payload");
            }
            return result;
        }
        // Discriminated evidence is copied only after inactive children have been proven empty.
        if (evidence.kind == FC_EVIDENCE_EXACT_BYTES || evidence.kind == FC_EVIDENCE_MASKED_BYTES) {
            auto bytes = copy_bytes(evidence.bytes);
            if (!bytes || bytes->empty() || evidence.target_rva != 0) {
                return std::unexpected("Byte evidence has an invalid expected payload");
            }
            result.bytes = std::move(*bytes);
            if (evidence.kind == FC_EVIDENCE_EXACT_BYTES) {
                if (!empty_byte_view(evidence.mask)) {
                    return std::unexpected("Exact evidence contains an inactive mask");
                }
                return result;
            }
            auto mask = copy_bytes(evidence.mask);
            if (!mask || mask->size() != result.bytes.size() || std::ranges::none_of(*mask, [](std::byte value) {
                    return value != std::byte{};
                })) {
                return std::unexpected("Masked evidence has an invalid or ineffective mask");
            }
            result.mask = std::move(*mask);
            return result;
        }
        if (evidence.kind == FC_EVIDENCE_POINTS_TO || evidence.kind == FC_EVIDENCE_DIRECT_CALL_TO ||
            evidence.kind == FC_EVIDENCE_DIRECT_JUMP_TO) {
            if (!empty_byte_view(evidence.bytes) || !empty_byte_view(evidence.mask)) {
                return std::unexpected("Relational evidence contains an inactive byte payload");
            }
            return result;
        }
        return std::unexpected("Evidence uses an unknown kind");
    }

    [[nodiscard]] std::expected<LocationRecord, std::string> copy_location(const FC_LocationView& location) {
        if (location.kind != FC_LOCATION_DATA && location.kind != FC_LOCATION_FUNCTION &&
            location.kind != FC_LOCATION_CODE) {
            return std::unexpected("Location uses an unknown semantic kind");
        }
        auto name = copy_string(location.name);
        auto label = copy_string(location.label);
        auto evidence = copy_evidence(location.evidence);
        if (!name || !label || !evidence) {
            return std::unexpected(!name ? name.error() : !label ? label.error() : evidence.error());
        }
        return LocationRecord{.kind = location.kind,
                              .rva = location.rva,
                              .name = std::move(*name),
                              .label = std::move(*label),
                              .evidence = std::move(*evidence)};
    }

    [[nodiscard]] std::expected<void, std::string> validate_evidence(const LocationRecord& location) const {
        return validate_location_evidence(image_, target_.architecture(), location, support_.image,
                                          baseline_.installed_hook_sites);
    }

    // A late participant reuses the first participant's decoded overwrite instead of decoding the active detour.
    [[nodiscard]] std::optional<std::uint32_t> installed_hook_overwrite(std::uint32_t rva) const noexcept {
        const auto installed = std::ranges::find_if(baseline_.installed_hook_sites, [&](const InstalledHookSite& site) {
            return site.image == support_.image && site.rva == rva;
        });
        return installed == baseline_.installed_hook_sites.end() ? std::nullopt
                                                                 : std::optional{installed->overwrite_size};
    }

    [[nodiscard]] bool
    add_evidence_claims(const LocationRecord& location, std::uint32_t operation,
                        std::optional<std::pair<std::uint32_t, std::uint64_t>> written = std::nullopt) {
        const auto size = evidence_extent(location.evidence, target_.architecture());
        if (size == 0) {
            return true;
        }
        const auto begin = static_cast<std::uint64_t>(location.rva);
        const auto end = begin + size;
        const auto add = [&](std::uint64_t claim_begin, std::uint64_t claim_end) {
            if (claim_begin < claim_end) {
                if (!charge(sizeof(MemoryClaim))) {
                    return false;
                }
                plan_.claims.push_back({patch_, support_.image, static_cast<std::uint32_t>(claim_begin),
                                        claim_end - claim_begin, ClaimAccess::Read, operation});
            }
            return true;
        };
        if (!written) {
            return add(begin, end);
        }
        // A mutation absorbs overlapping evidence; only the remaining preimage reads another patch's range.
        const auto write_begin = static_cast<std::uint64_t>(written->first);
        const auto write_end = write_begin + written->second;
        return add(begin, std::min(end, write_begin)) && add(std::max(begin, write_end), end);
    }

    [[nodiscard]] bool add_write_claim(std::uint32_t rva, std::uint64_t size, std::uint32_t operation) {
        const MemoryClaim claim{patch_, support_.image, rva, size, ClaimAccess::Write, operation};
        const auto overlap = std::ranges::find_if(plan_.claims, [&](const MemoryClaim& prior) {
            return prior.access == ClaimAccess::Write && claims_overlap(prior, claim);
        });
        if (overlap != plan_.claims.end()) {
            (void)reject("Distinct writes in one patch plan overlap");
            return false;
        }
        if (!charge(sizeof(MemoryClaim))) {
            return false;
        }
        plan_.claims.push_back(claim);
        return true;
    }

    [[nodiscard]] std::expected<AddressTargetRecord, std::string>
    copy_address_target(const FC_AddressTarget& target, bool executable, bool allow_data) const {
        AddressTargetRecord result{.kind = target.kind,
                                   .image_rva = target.image_rva,
                                   .data = target.data,
                                   .data_offset = target.data_offset,
                                   .plugin_function = target.plugin_function};
        // Image targets retain an RVA only and may additionally require reviewed executable target memory.
        if (target.kind == FC_ADDRESS_IMAGE) {
            if (target.data != FC_INVALID_DATA_HANDLE || target.data_offset != 0 || target.plugin_function != 0 ||
                target.image_rva >= image_.info().size ||
                (executable && !image_.is_executable({target.image_rva}, 1))) {
                return std::unexpected("Image address target has invalid active or inactive fields");
            }
            return result;
        }
        // Symbolic data is legal only when an operation permits a prior allocation owned by this patch plan.
        if (target.kind == FC_ADDRESS_DATA) {
            if (!allow_data || target.image_rva != 0 || target.data == FC_INVALID_DATA_HANDLE ||
                target.plugin_function != 0) {
                return std::unexpected("Symbolic data target is not valid for this operation");
            }
            const auto allocation = std::ranges::find_if(plan_.operations, [&](const OperationRecord& operation) {
                const auto* data = std::get_if<DataAllocationOperation>(&operation.payload);
                return data != nullptr && data->handle == target.data;
            });
            if (allocation == plan_.operations.end() ||
                target.data_offset > std::get<DataAllocationOperation>(allocation->payload).byte_size) {
                return std::unexpected("Symbolic data target does not belong to a prior allocation in the patch plan");
            }
            return result;
        }
        // Plugin function targets must contain no alternate representation and remain inside admitted executable code.
        if (target.kind == FC_ADDRESS_PLUGIN_FUNCTION) {
            if (target.image_rva != 0 || target.data != FC_INVALID_DATA_HANDLE || target.data_offset != 0 ||
                target.plugin_function == 0 || !owner_.contains_executable(target.plugin_function)) {
                return std::unexpected("Plugin function target is outside the submitting contribution's code owner");
            }
            return result;
        }
        return std::unexpected("Address target uses an unknown kind");
    }

    [[nodiscard]] bool valid_code_range(const LocationRecord& location, std::uint64_t size) const noexcept {
        return checked_extent(location.rva, size, image_.info().size) &&
               image_.is_executable({location.rva}, static_cast<std::size_t>(size));
    }

    [[nodiscard]] bool valid_data_range(const LocationRecord& location, std::uint64_t size) const noexcept {
        return checked_extent(location.rva, size, image_.info().size) &&
               image_.is_readable({location.rva}, static_cast<std::size_t>(size));
    }

    [[nodiscard]] bool has_hook_at(std::uint32_t rva) const noexcept {
        return std::ranges::any_of(plan_.operations, [&](const OperationRecord& operation) {
            const auto* hook = std::get_if<HookOperation>(&operation.payload);
            return hook != nullptr && hook->location.rva == rva;
        });
    }

    // Publishes an operation only after its payload, evidence, and claims have all been accepted.
    [[nodiscard]] FC_SubmitResult accept(OperationPayload payload) {
        if (!charge(sizeof(OperationRecord))) {
            return FC_SUBMIT_REJECTED;
        }
        plan_.operations.push_back(
            {.index = static_cast<std::uint32_t>(plan_.operations.size()), .payload = std::move(payload)});
        return FC_SUBMIT_ACCEPTED;
    }

    [[nodiscard]] FC_SubmitResult submit_require(const FC_RequireRequest& request, std::uintptr_t& output);
    [[nodiscard]] FC_SubmitResult submit_write(const FC_WriteRequest& request);
    [[nodiscard]] FC_SubmitResult submit_nop(const FC_NopRequest& request);
    [[nodiscard]] FC_SubmitResult submit_redirect(const FC_RedirectRequest& request, std::uintptr_t& output);
    [[nodiscard]] FC_SubmitResult submit_allocate(const FC_DataAllocationRequest& request, FC_DataHandle& output);
    [[nodiscard]] FC_SubmitResult submit_binding(const FC_InterfaceBindingRequest& request);
    [[nodiscard]] FC_SubmitResult submit_hook(const FC_HookRequest& request);
    [[nodiscard]] FC_SubmitResult submit_observer(const FC_ObserverRequest& request);

    // ABI trampolines validate pointers, contain exceptions, and preserve the collector's first failure.
    static FC_SubmitResult FC_CALL require_callback(void* context, const FC_RequireRequest* request,
                                                    std::uintptr_t* output) noexcept;
    static FC_SubmitResult FC_CALL write_callback(void* context, const FC_WriteRequest* request) noexcept;
    static FC_SubmitResult FC_CALL nop_callback(void* context, const FC_NopRequest* request) noexcept;
    static FC_SubmitResult FC_CALL redirect_callback(void* context, const FC_RedirectRequest* request,
                                                     std::uintptr_t* output) noexcept;
    static FC_SubmitResult FC_CALL allocate_callback(void* context, const FC_DataAllocationRequest* request,
                                                     FC_DataHandle* output) noexcept;
    static FC_SubmitResult FC_CALL binding_callback(void* context, const FC_InterfaceBindingRequest* request) noexcept;
    static FC_SubmitResult FC_CALL hook_callback(void* context, const FC_HookRequest* request) noexcept;
    static FC_SubmitResult FC_CALL observer_callback(void* context, const FC_ObserverRequest* request) noexcept;

    const targets::RecognizedTarget& target_;
    const catalog::Catalog& catalog_;
    catalog::PatchIndex patch_;
    const catalog::SupportDefinitionRecord& support_;
    const targets::ImageView& image_;
    const catalog::CodeOwner& owner_;
    ValidationBaseline baseline_;
    SubmittedPlan plan_;
    std::optional<std::string> failure_;
    const FC_PlanSink sink_{.struct_size = sizeof(FC_PlanSink),
                            .context = this,
                            .require = &require_callback,
                            .write = &write_callback,
                            .nop = &nop_callback,
                            .redirect = &redirect_callback,
                            .allocate_data = &allocate_callback,
                            .bind_interface = &binding_callback,
                            .hook = &hook_callback,
                            .observe = &observer_callback};
};

// Submit methods implement the operation matrix before publishing any callback output or accepting owned state.
FC_SubmitResult PlanCollector::submit_require(const FC_RequireRequest& request, std::uintptr_t& output) {
    constexpr auto required_size = offsetof(FC_RequireRequest, native_call) + sizeof(request.native_call);
    if (request.struct_size < required_size || request.size == 0 ||
        request.size > std::numeric_limits<std::size_t>::max() || !power_of_two(request.alignment) ||
        !canonical_boolean(request.writable)) {
        return reject("Native requirement has an invalid prefix, size, alignment, or Boolean");
    }
    auto location = copy_location(request.location);
    if (!location) {
        return reject(location.error());
    }
    if (auto evidence = validate_evidence(*location); !evidence) {
        return reject(evidence.error());
    }

    // The semantic location fixes both its reviewed access policy and whether a normalized call may be attached.
    std::optional<NativeCallRecord> native_call;
    if (location->kind == FC_LOCATION_DATA) {
        if (request.native_call != nullptr ||
            !(request.writable == FC_TRUE ? image_.is_writable({location->rva}, static_cast<std::size_t>(request.size))
                                          : valid_data_range(*location, request.size))) {
            return reject("Data requirement violates its call, access, or reviewed range contract");
        }
    } else if (location->kind == FC_LOCATION_CODE) {
        if (request.native_call != nullptr || request.writable != FC_FALSE || request.alignment != 1 ||
            !valid_code_range(*location, request.size)) {
            return reject("Requirement for code access violates its access, alignment, or executable range contract");
        }
    } else {
        if (request.writable != FC_FALSE || request.alignment != 1 || !valid_code_range(*location, request.size)) {
            return reject("Function requirement violates its access, alignment, or executable range contract");
        }
        if (request.native_call != nullptr) {
            constexpr auto native_required =
                offsetof(FC_NativeCall, stack_size) + sizeof(request.native_call->stack_size);
            if (request.native_call->struct_size < native_required) {
                return reject("Native call record has an invalid prefix");
            }
            if (request.native_call->argument_count > kPatchPlanByteCapacity / sizeof(FC_NativeArgument)) {
                return reject("Native call record exceeds the budget for the patch plan");
            }
            const auto native_bytes =
                sizeof(NativeCallRecord) +
                static_cast<std::size_t>(request.native_call->argument_count) * sizeof(FC_NativeArgument);
            if (!charge(native_bytes)) {
                return FC_SUBMIT_REJECTED;
            }
            auto copied = validate_native_call(*request.native_call, target_.architecture());
            if (!copied) {
                return reject(copied.error());
            }
            native_call.emplace(std::move(*copied));
        }
    }

    if (image_.info().base > std::numeric_limits<std::uintptr_t>::max() - location->rva) {
        return reject("Resolved requirement address overflows");
    }
    // Resolve and claim the requirement now so the callback receives only an address reviewed against this image.
    const auto address = image_.info().base + location->rva;
    if (address % request.alignment != 0) {
        return reject("Resolved requirement address does not satisfy its declared alignment");
    }
    const auto operation = static_cast<std::uint32_t>(plan_.operations.size());
    const auto access = request.writable == FC_TRUE ? ClaimAccess::Write : ClaimAccess::Read;
    if (access == ClaimAccess::Write) {
        if (!add_write_claim(location->rva, request.size, operation)) {
            return FC_SUBMIT_REJECTED;
        }
    } else {
        if (!charge(sizeof(MemoryClaim))) {
            return FC_SUBMIT_REJECTED;
        }
        plan_.claims.push_back({patch_, support_.image, location->rva, request.size, access, operation});
    }
    if (!add_evidence_claims(*location, operation,
                             access == ClaimAccess::Write ? std::optional{std::pair{location->rva, request.size}}
                                                          : std::nullopt)) {
        return FC_SUBMIT_REJECTED;
    }
    RequireOperation operation_record{.location = std::move(*location),
                                      .size = request.size,
                                      .alignment = request.alignment,
                                      .writable = request.writable == FC_TRUE,
                                      .native_call = std::move(native_call),
                                      .resolved_address = address};
    if (accept(std::move(operation_record)) != FC_SUBMIT_ACCEPTED) {
        return FC_SUBMIT_REJECTED;
    }
    output = address;
    return FC_SUBMIT_ACCEPTED;
}

FC_SubmitResult PlanCollector::submit_write(const FC_WriteRequest& request) {
    // Copy and validate the semantic location before interpreting any payload or target from plugin memory.
    constexpr auto required_size = offsetof(FC_WriteRequest, target) + sizeof(request.target);
    if (request.struct_size < required_size) {
        return reject("Native write has an invalid record prefix");
    }
    auto location = copy_location(request.location);
    if (!location) {
        return reject(location.error());
    }
    if (auto evidence = validate_evidence(*location); !evidence) {
        return reject(evidence.error());
    }

    // The discriminant selects one payload form and the exact destination extent claimed by this operation.
    std::uint64_t extent{};
    std::vector<std::byte> bytes;
    AddressTargetRecord target;
    if (request.kind == FC_WRITE_BYTES) {
        if (request.bytes.size == 0 || !empty_address_target(request.target)) {
            return reject("Literal write requires nonempty bytes and an empty address target");
        }
        auto copied = copy_bytes(request.bytes);
        if (!copied) {
            return reject(copied.error());
        }
        bytes = std::move(*copied);
        extent = bytes.size();
        if (location->kind != FC_LOCATION_DATA && location->kind != FC_LOCATION_CODE) {
            return reject("Literal write requires a location with data or code access");
        }
    } else {
        if (!empty_byte_view(request.bytes)) {
            return reject("Address write contains an inactive byte payload");
        }
        bool executable_target{};
        bool allow_data = true;
        if (request.kind == FC_WRITE_POINTER) {
            if (location->kind != FC_LOCATION_DATA) {
                return reject("Pointer write requires a Data location");
            }
            extent = pointer_size(target_.architecture());
        } else if (request.kind == FC_WRITE_REL32) {
            if (location->kind != FC_LOCATION_DATA && location->kind != FC_LOCATION_CODE) {
                return reject("Relative displacement write requires a location with data or code access");
            }
            extent = 4;
        } else if (request.kind == FC_WRITE_CALL || request.kind == FC_WRITE_JUMP) {
            if (location->kind != FC_LOCATION_CODE) {
                return reject("Direct branch write requires a location with code access");
            }
            extent = 5;
            executable_target = true;
            allow_data = false;
        } else {
            return reject("Native write uses an unknown kind");
        }
        auto copied = copy_address_target(request.target, executable_target, allow_data);
        if (!copied) {
            return reject(copied.error());
        }
        target = *copied;

        // Symbolic data and branch relays are placed later; only targets needing a fixed displacement close now.
        const bool fixed_rel32 = request.kind == FC_WRITE_REL32 && target.kind != FC_ADDRESS_DATA;
        const bool fixed_branch =
            (request.kind == FC_WRITE_CALL || request.kind == FC_WRITE_JUMP) && target.kind == FC_ADDRESS_IMAGE;
        if (fixed_rel32 || fixed_branch) {
            const auto destination =
                target.kind == FC_ADDRESS_IMAGE ? image_.info().base + target.image_rva : target.plugin_function;
            const auto next = image_.info().base + location->rva + static_cast<std::uintptr_t>(extent);
            if (!rel32_reachable(next, destination, target_.architecture())) {
                return reject("Fixed address target is outside signed rel32 reach");
            }
        }
    }
    // The settled payload determines the one destination claim and the evidence range excluded from duplicate claims.
    const bool valid_range =
        location->kind == FC_LOCATION_CODE ? valid_code_range(*location, extent) : valid_data_range(*location, extent);
    if (!valid_range) {
        return reject("Native write destination is outside its reviewed image range");
    }
    const auto operation = static_cast<std::uint32_t>(plan_.operations.size());
    if (!add_write_claim(location->rva, extent, operation)) {
        return FC_SUBMIT_REJECTED;
    }
    if (!add_evidence_claims(*location, operation, std::pair{location->rva, extent})) {
        return FC_SUBMIT_REJECTED;
    }
    return accept(WriteOperation{
        .location = std::move(*location), .kind = request.kind, .bytes = std::move(bytes), .target = target});
}

FC_SubmitResult PlanCollector::submit_nop(const FC_NopRequest& request) {
    // Validate and deep-copy the complete request before deriving claims from any plugin-owned location metadata.
    constexpr auto required_size = offsetof(FC_NopRequest, size) + sizeof(request.size);
    if (request.struct_size < required_size || request.size == 0) {
        return reject("NOP request has an invalid prefix or empty extent");
    }
    auto location = copy_location(request.location);
    if (!location) {
        return reject(location.error());
    }
    if (location->kind != FC_LOCATION_CODE || !valid_code_range(*location, request.size)) {
        return reject("NOP request requires a complete executable range with code access");
    }
    if (auto evidence = validate_evidence(*location); !evidence) {
        return reject(evidence.error());
    }
    // A successful operation owns both its mutation range and any non-overlapping evidence preimage.
    const auto operation = static_cast<std::uint32_t>(plan_.operations.size());
    if (!add_write_claim(location->rva, request.size, operation)) {
        return FC_SUBMIT_REJECTED;
    }
    if (!add_evidence_claims(*location, operation, std::pair{location->rva, request.size})) {
        return FC_SUBMIT_REJECTED;
    }
    return accept(NopOperation{.location = std::move(*location), .size = request.size});
}

FC_SubmitResult PlanCollector::submit_redirect(const FC_RedirectRequest& request, std::uintptr_t& output) {
    constexpr auto required_size = offsetof(FC_RedirectRequest, target) + sizeof(request.target);
    if (request.struct_size < required_size || (request.kind != FC_REDIRECT_CALL && request.kind != FC_REDIRECT_JUMP)) {
        return reject("Redirect request has an invalid prefix or kind");
    }
    auto location = copy_location(request.location);
    if (!location) {
        return reject(location.error());
    }
    if (location->kind != FC_LOCATION_CODE || !valid_code_range(*location, 5)) {
        return reject("Redirect requires a five-byte executable range with code access");
    }
    if (auto evidence = validate_evidence(*location); !evidence) {
        return reject(evidence.error());
    }
    auto target = copy_address_target(request.target, true, false);
    if (!target) {
        return reject(target.error());
    }
    // Redirects capture the current direct target; inserted branches have no contract for an Original call.
    std::array<std::byte, 5> instruction{};
    if (!image_.read({location->rva}, instruction)) {
        return reject("Existing direct branch could not be read");
    }
    const auto opcode = request.kind == FC_REDIRECT_CALL ? std::byte{0xe8} : std::byte{0xe9};
    if (instruction.front() != opcode) {
        return reject("Redirect site is not the selected direct branch form");
    }
    std::int32_t displacement{};
    std::memcpy(&displacement, instruction.data() + 1, sizeof(displacement));
    const auto next = image_.info().base + location->rva + instruction.size();
    const auto original = decode_rel32_target(next, displacement, target_.architecture());
    if (!original) {
        return reject("Decoded original branch target overflows the selected address width");
    }
    const auto destination =
        target->kind == FC_ADDRESS_IMAGE ? image_.info().base + target->image_rva : target->plugin_function;
    if (target->kind == FC_ADDRESS_IMAGE && !rel32_reachable(next, destination, target_.architecture())) {
        return reject("Redirect replacement is outside signed rel32 reach");
    }
    // Claim the instruction and return its decoded destination only after the complete redirect is retained.
    const auto operation = static_cast<std::uint32_t>(plan_.operations.size());
    if (!add_write_claim(location->rva, instruction.size(), operation)) {
        return FC_SUBMIT_REJECTED;
    }
    if (!add_evidence_claims(*location, operation, std::pair{location->rva, std::uint64_t{instruction.size()}})) {
        return FC_SUBMIT_REJECTED;
    }
    if (accept(RedirectOperation{
            .location = std::move(*location), .kind = request.kind, .target = *target, .original_target = *original}) !=
        FC_SUBMIT_ACCEPTED) {
        return FC_SUBMIT_REJECTED;
    }
    output = *original;
    return FC_SUBMIT_ACCEPTED;
}

FC_SubmitResult PlanCollector::submit_allocate(const FC_DataAllocationRequest& request, FC_DataHandle& output) {
    // Reject malformed extents and views before copying bytes or issuing a symbolic handle.
    constexpr auto required_size = offsetof(FC_DataAllocationRequest, name) + sizeof(request.name);
    if (request.struct_size < required_size || request.byte_size == 0 ||
        request.byte_size > std::numeric_limits<std::size_t>::max() || !power_of_two(request.alignment) ||
        !valid_byte_view(request.initial_bytes) ||
        (request.initial_bytes.size != 0 && request.initial_bytes.size != request.byte_size)) {
        return reject("Data allocation has an invalid prefix, extent, alignment, or initial payload");
    }
    auto initial = copy_bytes(request.initial_bytes);
    auto name = copy_string(request.name);
    if (!initial || !name) {
        return reject(!initial ? initial.error() : name.error());
    }
    if (plan_.operations.size() >= std::numeric_limits<FC_DataHandle>::max()) {
        return reject("Patch plan exhausted its available symbolic data handles");
    }
    // Handles are operation ordinals local to the patch plan and become visible after allocation is accepted.
    const auto handle = static_cast<FC_DataHandle>(plan_.operations.size() + 1);
    if (accept(DataAllocationOperation{.handle = handle,
                                       .byte_size = request.byte_size,
                                       .alignment = request.alignment,
                                       .initial_bytes = std::move(*initial),
                                       .name = std::move(*name)}) != FC_SUBMIT_ACCEPTED) {
        return FC_SUBMIT_REJECTED;
    }
    output = handle;
    return FC_SUBMIT_ACCEPTED;
}

FC_SubmitResult PlanCollector::submit_binding(const FC_InterfaceBindingRequest& request) {
    // The copied route retains only exact interface layout metadata and this plugin's executable connection thunk.
    constexpr auto required_size = offsetof(FC_InterfaceBindingRequest, connect) + sizeof(request.connect);
    if (request.struct_size < required_size || request.size == 0 || request.size > kInterfaceByteCapacity ||
        request.connect == nullptr || !owner_.contains_executable(reinterpret_cast<std::uintptr_t>(request.connect))) {
        return reject("Interface binding has an invalid prefix, size, or connection callback");
    }
    auto provider = copy_string(request.provider_patch);
    auto id = copy_string(request.id);
    if (!provider || !id || !catalog::valid_framework_id(*provider) || !catalog::valid_framework_id(*id)) {
        return reject("Interface binding contains an invalid provider or interface ID");
    }
    return accept(InterfaceBindingOperation{.provider_patch = std::move(*provider),
                                            .id = std::move(*id),
                                            .size = request.size,
                                            .context = request.context,
                                            .connect = request.connect});
}

[[nodiscard]] std::expected<std::uint32_t, std::string> derive_hook_overwrite(const targets::ImageView& image,
                                                                              FC_Architecture architecture,
                                                                              const LocationRecord& location,
                                                                              FC_HookKind kind) {
    // A hook at a call site replaces one instruction; entry and instruction hooks must preserve whole instructions.
    if (kind == FC_HOOK_DIRECT_CALL_SITE) {
        std::array<std::byte, 5> instruction{};
        if (!image.is_executable({location.rva}, instruction.size()) || !image.read({location.rva}, instruction) ||
            instruction.front() != std::byte{0xe8}) {
            return std::unexpected("Direct call hook site is not a five-byte executable direct call");
        }
        return static_cast<std::uint32_t>(instruction.size());
    }

    const auto remaining = image.info().size - std::min<std::size_t>(location.rva, image.info().size);
    auto read_size = std::min(kHookDecodeByteCapacity, remaining);
    // A hook near a reviewed section boundary needs only enough whole instructions for its overwrite, not 64 bytes.
    while (read_size >= kMinimumHookOverwrite && !image.is_executable({location.rva}, read_size)) {
        --read_size;
    }
    if (read_size < kMinimumHookOverwrite) {
        return std::unexpected("Hook site does not contain enough reviewed executable bytes");
    }
    std::array<std::byte, kHookDecodeByteCapacity> bytes{};
    if (!image.read({location.rva}, std::span{bytes}.first(read_size))) {
        return std::unexpected("Hook overwrite bytes could not be read");
    }
    ZydisDecoder decoder{};
    const auto machine = architecture == FC_ARCH_X86 ? ZYDIS_MACHINE_MODE_LONG_COMPAT_32 : ZYDIS_MACHINE_MODE_LONG_64;
    const auto width = architecture == FC_ARCH_X86 ? ZYDIS_STACK_WIDTH_32 : ZYDIS_STACK_WIDTH_64;
    if (ZYAN_FAILED(ZydisDecoderInit(&decoder, machine, width))) {
        return std::unexpected("Zydis could not initialize for the selected architecture");
    }
    std::uint32_t extent{};
    while (extent < kMinimumHookOverwrite) {
        ZydisDecodedInstruction instruction{};
        if (extent >= read_size ||
            ZYAN_FAILED(ZydisDecoderDecodeInstruction(&decoder, nullptr, bytes.data() + extent, read_size - extent,
                                                      &instruction)) ||
            instruction.length == 0) {
            return std::unexpected("Hook overwrite does not end on a decodable instruction boundary");
        }
        extent += instruction.length;
    }
    return extent;
}

FC_SubmitResult PlanCollector::submit_hook(const FC_HookRequest& request) {
    // Validate retained plugin code and copy the site description before deriving physical hook requirements.
    constexpr auto required_size = offsetof(FC_HookRequest, bind_original) + sizeof(request.bind_original);
    if (request.struct_size < required_size || request.builder.build == nullptr || request.builder.entry_size == 0 ||
        request.builder.entry_size > FC_HOOK_ENTRY_MAX_SIZE ||
        !owner_.contains_executable(reinterpret_cast<std::uintptr_t>(request.builder.build)) || request.callback == 0 ||
        !owner_.contains_executable(request.callback)) {
        return reject("Hook owner has an invalid prefix, builder, entry extent, or callback");
    }
    auto location = copy_location(request.location);
    if (!location) {
        return reject(location.error());
    }
    if (auto evidence = validate_evidence(*location); !evidence) {
        return reject(evidence.error());
    }
    if ((request.kind == FC_HOOK_FUNCTION_ENTRY && location->kind != FC_LOCATION_FUNCTION) ||
        ((request.kind == FC_HOOK_DIRECT_CALL_SITE || request.kind == FC_HOOK_INSTRUCTION) &&
         location->kind != FC_LOCATION_CODE) ||
        (request.kind != FC_HOOK_FUNCTION_ENTRY && request.kind != FC_HOOK_DIRECT_CALL_SITE &&
         request.kind != FC_HOOK_INSTRUCTION)) {
        return reject("Hook kind does not match its semantic location");
    }
    if (has_hook_at(location->rva)) {
        return reject("One patch may contribute only once at a physical hook site");
    }
    // Late participants inherit an installed site's immutable overwrite extent; new sites decode whole instructions.
    const auto installed_overwrite = installed_hook_overwrite(location->rva);
    auto overwrite = installed_overwrite
                         ? std::expected<std::uint32_t, std::string>{*installed_overwrite}
                         : derive_hook_overwrite(image_, target_.architecture(), *location, request.kind);
    if (!overwrite) {
        return reject(overwrite.error());
    }

    // Instruction owners receive raw CPU context; typed sites instead require the full call and Original binding.
    std::optional<NativeCallRecord> native_call;
    if (request.kind == FC_HOOK_INSTRUCTION) {
        if (request.native_call != nullptr || request.original_context != nullptr || request.bind_original != nullptr) {
            return reject("Instruction hook contains fields for a typed call or Original binding");
        }
    } else {
        if (request.native_call == nullptr || request.bind_original == nullptr ||
            !owner_.contains_executable(reinterpret_cast<std::uintptr_t>(request.bind_original))) {
            return reject("Typed hook requires a native call and a thunk that owns its Original binding");
        }
        constexpr auto native_required = offsetof(FC_NativeCall, stack_size) + sizeof(request.native_call->stack_size);
        if (request.native_call->struct_size < native_required) {
            return reject("Hook native call record has an invalid prefix");
        }
        if (request.native_call->argument_count > kPatchPlanByteCapacity / sizeof(FC_NativeArgument)) {
            return reject("Hook native call exceeds the budget for the patch plan");
        }
        const auto native_bytes =
            sizeof(NativeCallRecord) +
            static_cast<std::size_t>(request.native_call->argument_count) * sizeof(FC_NativeArgument);
        if (!charge(native_bytes)) {
            return FC_SUBMIT_REJECTED;
        }
        auto copied = validate_native_call(*request.native_call, target_.architecture());
        if (!copied) {
            return reject(copied.error());
        }
        native_call.emplace(std::move(*copied));
    }
    // Evidence is retained as an ordinary claim before the complete owner request enters the patch plan.
    if (!add_evidence_claims(*location, static_cast<std::uint32_t>(plan_.operations.size()),
                             std::pair{location->rva, static_cast<std::uint64_t>(*overwrite)})) {
        return FC_SUBMIT_REJECTED;
    }
    return accept(HookOperation{.location = std::move(*location),
                                .kind = request.kind,
                                .overwrite_size = *overwrite,
                                .native_call = std::move(native_call),
                                .builder = request.builder,
                                .observer = false,
                                .context = request.context,
                                .callback = request.callback,
                                .original_context = request.original_context,
                                .bind_original = request.bind_original});
}

FC_SubmitResult PlanCollector::submit_observer(const FC_ObserverRequest& request) {
    // Validate every retained callback before copying site metadata or admitting optional per-invocation state.
    constexpr auto required_size = offsetof(FC_ObserverRequest, state_alignment) + sizeof(request.state_alignment);
    if (request.struct_size < required_size || request.builder.build == nullptr || request.builder.entry_size == 0 ||
        request.builder.entry_size > FC_HOOK_ENTRY_MAX_SIZE ||
        !owner_.contains_executable(reinterpret_cast<std::uintptr_t>(request.builder.build)) ||
        (request.before == 0 && request.after == 0) ||
        (request.before != 0 && !owner_.contains_executable(request.before)) ||
        (request.after != 0 && !owner_.contains_executable(request.after))) {
        return reject("Hook observer has an invalid prefix, builder, entry extent, or callback");
    }
    // Invocation-local state is meaningful only for a paired before/after observer and joins the site capacity later.
    if ((request.state_size == 0 && request.state_alignment != 0) ||
        (request.state_size != 0 &&
         (request.before == 0 || request.after == 0 || !power_of_two(request.state_alignment) ||
          request.state_alignment > kHookStateAlignmentCapacity))) {
        return reject("Hook observer state size, alignment, and callback pairing are inconsistent");
    }
    auto location = copy_location(request.location);
    if (!location) {
        return reject(location.error());
    }
    if (auto evidence = validate_evidence(*location); !evidence) {
        return reject(evidence.error());
    }
    if ((request.kind == FC_HOOK_FUNCTION_ENTRY && location->kind != FC_LOCATION_FUNCTION) ||
        ((request.kind == FC_HOOK_DIRECT_CALL_SITE || request.kind == FC_HOOK_INSTRUCTION) &&
         location->kind != FC_LOCATION_CODE) ||
        (request.kind != FC_HOOK_FUNCTION_ENTRY && request.kind != FC_HOOK_DIRECT_CALL_SITE &&
         request.kind != FC_HOOK_INSTRUCTION)) {
        return reject("Observer hook kind does not match its semantic location");
    }
    if (has_hook_at(location->rva)) {
        return reject("One patch may contribute only once at a physical hook site");
    }
    // Observers join an installed baseline without decoding a second physical overwrite at the same site.
    const auto installed_overwrite = installed_hook_overwrite(location->rva);
    auto overwrite = installed_overwrite
                         ? std::expected<std::uint32_t, std::string>{*installed_overwrite}
                         : derive_hook_overwrite(image_, target_.architecture(), *location, request.kind);
    if (!overwrite) {
        return reject(overwrite.error());
    }

    // Typed observers share the site's normalized call; instruction observers operate only on CPU context.
    std::optional<NativeCallRecord> native_call;
    if (request.kind == FC_HOOK_INSTRUCTION) {
        if (request.native_call != nullptr) {
            return reject("Instruction observer contains a typed native call");
        }
    } else {
        if (request.native_call == nullptr) {
            return reject("Typed observer requires a complete native call");
        }
        constexpr auto native_required = offsetof(FC_NativeCall, stack_size) + sizeof(request.native_call->stack_size);
        if (request.native_call->struct_size < native_required) {
            return reject("Observer native call record has an invalid prefix");
        }
        if (request.native_call->argument_count > kPatchPlanByteCapacity / sizeof(FC_NativeArgument)) {
            return reject("Observer native call exceeds the budget for the patch plan");
        }
        const auto native_bytes =
            sizeof(NativeCallRecord) +
            static_cast<std::size_t>(request.native_call->argument_count) * sizeof(FC_NativeArgument);
        if (!charge(native_bytes)) {
            return FC_SUBMIT_REJECTED;
        }
        auto copied = validate_native_call(*request.native_call, target_.architecture());
        if (!copied) {
            return reject(copied.error());
        }
        native_call.emplace(std::move(*copied));
    }
    // The copied observer and its evidence become one plan operation only after shape and state checks succeed.
    if (!add_evidence_claims(*location, static_cast<std::uint32_t>(plan_.operations.size()),
                             std::pair{location->rva, static_cast<std::uint64_t>(*overwrite)})) {
        return FC_SUBMIT_REJECTED;
    }
    return accept(HookOperation{.location = std::move(*location),
                                .kind = request.kind,
                                .overwrite_size = *overwrite,
                                .native_call = std::move(native_call),
                                .builder = request.builder,
                                .observer = true,
                                .context = request.context,
                                .callback = request.before,
                                .after = request.after,
                                .state_size = request.state_size,
                                .state_alignment = request.state_alignment});
}

FC_SubmitResult FC_CALL PlanCollector::require_callback(void* context, const FC_RequireRequest* request,
                                                        std::uintptr_t* output) noexcept {
    // Every ABI trampoline zeroes outputs first and contains C++ exceptions inside the native callback boundary.
    if (output != nullptr) {
        *output = 0;
    }
    if (context == nullptr) {
        return FC_SUBMIT_REJECTED;
    }
    auto& self = *static_cast<PlanCollector*>(context);
    if (request == nullptr || output == nullptr || self.failed()) {
        return self.reject("Require submission has a null input/output or follows an earlier plan failure");
    }
    try {
        return self.submit_require(*request, *output);
    } catch (...) {
        return self.reject("Require submission threw while the framework copied or validated it");
    }
}

FC_SubmitResult FC_CALL PlanCollector::write_callback(void* context, const FC_WriteRequest* request) noexcept {
    if (context == nullptr) {
        return FC_SUBMIT_REJECTED;
    }
    auto& self = *static_cast<PlanCollector*>(context);
    if (request == nullptr || self.failed()) {
        return self.reject("Write submission is null or follows an earlier plan failure");
    }
    try {
        return self.submit_write(*request);
    } catch (...) {
        return self.reject("Write submission threw while the framework copied or validated it");
    }
}

FC_SubmitResult FC_CALL PlanCollector::nop_callback(void* context, const FC_NopRequest* request) noexcept {
    if (context == nullptr) {
        return FC_SUBMIT_REJECTED;
    }
    auto& self = *static_cast<PlanCollector*>(context);
    if (request == nullptr || self.failed()) {
        return self.reject("NOP submission is null or follows an earlier plan failure");
    }
    try {
        return self.submit_nop(*request);
    } catch (...) {
        return self.reject("NOP submission threw while the framework copied or validated it");
    }
}

FC_SubmitResult FC_CALL PlanCollector::redirect_callback(void* context, const FC_RedirectRequest* request,
                                                         std::uintptr_t* output) noexcept {
    if (output != nullptr) {
        *output = 0;
    }
    if (context == nullptr) {
        return FC_SUBMIT_REJECTED;
    }
    auto& self = *static_cast<PlanCollector*>(context);
    if (request == nullptr || output == nullptr || self.failed()) {
        return self.reject("Redirect submission has a null input/output or follows an earlier plan failure");
    }
    try {
        return self.submit_redirect(*request, *output);
    } catch (...) {
        return self.reject("Redirect submission threw while the framework copied or validated it");
    }
}

FC_SubmitResult FC_CALL PlanCollector::allocate_callback(void* context, const FC_DataAllocationRequest* request,
                                                         FC_DataHandle* output) noexcept {
    if (output != nullptr) {
        *output = FC_INVALID_DATA_HANDLE;
    }
    if (context == nullptr) {
        return FC_SUBMIT_REJECTED;
    }
    auto& self = *static_cast<PlanCollector*>(context);
    if (request == nullptr || output == nullptr || self.failed()) {
        return self.reject("Allocation submission has a null input/output or follows an earlier plan failure");
    }
    try {
        return self.submit_allocate(*request, *output);
    } catch (...) {
        return self.reject("Allocation submission threw while the framework copied or validated it");
    }
}

FC_SubmitResult FC_CALL PlanCollector::binding_callback(void* context,
                                                        const FC_InterfaceBindingRequest* request) noexcept {
    if (context == nullptr) {
        return FC_SUBMIT_REJECTED;
    }
    auto& self = *static_cast<PlanCollector*>(context);
    if (request == nullptr || self.failed()) {
        return self.reject("Binding submission is null or follows an earlier plan failure");
    }
    try {
        return self.submit_binding(*request);
    } catch (...) {
        return self.reject("Binding submission threw while the framework copied or validated it");
    }
}

FC_SubmitResult FC_CALL PlanCollector::hook_callback(void* context, const FC_HookRequest* request) noexcept {
    if (context == nullptr) {
        return FC_SUBMIT_REJECTED;
    }
    auto& self = *static_cast<PlanCollector*>(context);
    if (request == nullptr || self.failed()) {
        return self.reject("Hook submission is null or follows an earlier plan failure");
    }
    try {
        return self.submit_hook(*request);
    } catch (...) {
        return self.reject("Hook submission threw while the framework copied or validated it");
    }
}

FC_SubmitResult FC_CALL PlanCollector::observer_callback(void* context, const FC_ObserverRequest* request) noexcept {
    if (context == nullptr) {
        return FC_SUBMIT_REJECTED;
    }
    auto& self = *static_cast<PlanCollector*>(context);
    if (request == nullptr || self.failed()) {
        return self.reject("Observer submission is null or follows an earlier plan failure");
    }
    try {
        return self.submit_observer(*request);
    } catch (...) {
        return self.reject("Observer submission threw while the framework copied or validated it");
    }
}

void fail_patch(PatchWorkSet& patches, catalog::PatchIndex patch, std::string message, PatchPhase phase,
                std::string operation) {
    auto& record = patches.record(patch);
    if (record.state == PatchState::Pending || record.state == PatchState::Ready ||
        record.state == PatchState::WaitingForImage) {
        finish_inactive_patch(
            record, PatchState::Failed,
            FailureReason{.message = std::move(message), .phase = phase, .operation = std::move(operation)});
    }
}

[[nodiscard]] std::vector<catalog::PatchIndex> sorted_pending(const PatchWorkSet& patches) {
    std::vector<catalog::PatchIndex> result;
    for (const auto& record : patches.records()) {
        if (record.state == PatchState::Pending) {
            result.push_back(record.patch);
        }
    }
    std::ranges::sort(result, [&](catalog::PatchIndex left, catalog::PatchIndex right) {
        const auto left_id = catalog::fold_ascii(patches.catalog().patch(left).id);
        const auto right_id = catalog::fold_ascii(patches.catalog().patch(right).id);
        return left_id != right_id ? left_id < right_id
                                   : patches.catalog().patch(left).id < patches.catalog().patch(right).id;
    });
    return result;
}

// Create and Plan callbacks run in deterministic ID order so shared capacity cannot depend on contribution order.
void collect_patch_plans(const targets::RecognizedTarget& target, PatchWorkSet& patches, ValidationBaseline baseline) {
    std::size_t retained_total{};
    for (const auto patch : sorted_pending(patches)) {
        auto& record = patches.record(patch);
        const auto& support = selected_support(patches.catalog(), patch);
        const auto& callbacks = support.callbacks;
        // Even an empty schema crosses as one non-null, prefix-valid settings view for the Create contract.
        const auto native_values = record.settings.native_values();
        const FC_SettingsView settings{.struct_size = sizeof(FC_SettingsView),
                                       .values = native_values.empty() ? nullptr : native_values.data(),
                                       .count = static_cast<std::uint32_t>(native_values.size())};
        const FC_CreateContext create{.struct_size = sizeof(FC_CreateContext),
                                      .report = catalog::report_token(patch),
                                      .target = target_info(target, support.image)};
        catalog::CallbackError callback_error;
        const auto error = callback_error.sink();
        FC_PatchHandle output{};
        FC_CallStatus status = FC_CALL_FAILED;
        bool threw{};
        // Create crosses the plugin boundary first and transfers an instance to the framework only on success.
        try {
            status = callbacks.create(callbacks.context, &create, &settings, &error, &output);
        } catch (...) {
            threw = true;
        }
        if (threw || status != FC_CALL_OK || output == nullptr) {
            std::string message;
            if (threw) {
                message = "Create callback threw across the native boundary";
            } else if (status == FC_CALL_OK) {
                message = "Successful Create callback returned a null patch handle";
            } else if (output != nullptr) {
                message = "Failed Create callback returned a patch handle that appears to own state";
            } else {
                message = callback_error.supplied && !callback_error.message.empty() ? callback_error.message
                                                                                     : "Patch Create callback failed";
            }
            fail_patch(patches, patch, std::move(message), PatchPhase::Create,
                       callback_error.operation.empty() ? "Create patch instance" : callback_error.operation);
            continue;
        }
        record.instance = PatchInstance{callbacks, output};
        // Successful SDK construction owns the typed values; the generic flat transport can now be released.
        record.settings = {};

        PlanCollector collector{target, patches.catalog(), patch, baseline};
        const FC_PlanContext plan_context{.struct_size = sizeof(FC_PlanContext),
                                          .report = catalog::report_token(patch),
                                          .target = target_info(target, support.image)};
        callback_error = {};
        status = FC_CALL_FAILED;
        threw = false;
        // Submissions from the Plan callback are copied synchronously, so borrowed request storage cannot survive.
        try {
            status = callbacks.plan(callbacks.context, record.instance.get(), &plan_context, &collector.sink(), &error);
        } catch (...) {
            threw = true;
        }
        if (threw || status != FC_CALL_OK || collector.failed()) {
            std::string message;
            if (collector.failed()) {
                message = collector.failure();
            } else if (threw) {
                message = "Plan callback threw across the native boundary";
            } else {
                message = callback_error.supplied && !callback_error.message.empty() ? callback_error.message
                                                                                     : "Patch's Plan callback failed";
            }
            fail_patch(patches, patch, std::move(message), PatchPhase::Plan,
                       callback_error.operation.empty() ? "Build patch plan" : callback_error.operation);
            continue;
        }
        auto submitted = std::move(collector).take_plan();
        // Target timing policy is ordinary validation: a direct defect fails only this patch and its consumers.
        auto late_policy = validate_late_mutation_policy(target, support.image, submitted);
        if (!late_policy) {
            fail_patch(patches, patch, std::move(late_policy.error()), PatchPhase::Validation,
                       "Validate mutation safety for a late image");
            continue;
        }
        // One validation budget bounds simultaneous ownership across every patch plan still eligible for installation.
        if (submitted.retained_bytes > kValidationPlanByteCapacity - retained_total) {
            fail_patch(patches, patch, "Validation run exceeds the shared copied plan capacity", PatchPhase::Plan,
                       "Retain patch plan");
            continue;
        }
        retained_total += submitted.retained_bytes;
        record.plan = std::move(submitted);
    }
}

// Points back to one mutable hook operation while validation of each site groups contributions from several patches.
struct HookReference {
    catalog::PatchIndex patch;
    std::uint32_t operation_index{};
    HookOperation* operation{};
};

// A physical image address is the aggregation identity for owners and observers sharing one hook installation.
struct HookSiteGroup {
    FC_TargetImage image{};
    std::uint32_t rva{};
    std::vector<HookReference> participants;
};

// Collects pending hook contributions into deterministic site and participant order for simultaneous validation.
[[nodiscard]] std::vector<HookSiteGroup> collect_hook_groups(PatchWorkSet& patches) {
    std::vector<HookSiteGroup> groups;
    for (auto& record : patches.records()) {
        if (record.state != PatchState::Pending) {
            continue;
        }
        const auto image = selected_support(patches.catalog(), record.patch).image;
        for (auto& operation_record : record.plan.operations) {
            auto* hook = std::get_if<HookOperation>(&operation_record.payload);
            if (hook == nullptr) {
                continue;
            }
            const auto found = std::ranges::find_if(groups, [&](const HookSiteGroup& group) {
                return group.image == image && group.rva == hook->location.rva;
            });
            if (found == groups.end()) {
                groups.push_back({.image = image,
                                  .rva = hook->location.rva,
                                  .participants = {{record.patch, operation_record.index, hook}}});
            } else {
                found->participants.push_back({record.patch, operation_record.index, hook});
            }
        }
    }
    // Stable ordering by patch ID and operation makes later owner selection and capacity failures reproducible.
    for (auto& group : groups) {
        std::ranges::sort(group.participants, [&](const HookReference& left, const HookReference& right) {
            const auto left_id = catalog::fold_ascii(patches.catalog().patch(left.patch).id);
            const auto right_id = catalog::fold_ascii(patches.catalog().patch(right.patch).id);
            return left_id != right_id ? left_id < right_id : left.operation_index < right.operation_index;
        });
    }
    return groups;
}

[[nodiscard]] bool compatible_hook(const HookOperation& left, const HookOperation& right) noexcept {
    if (left.kind != right.kind || left.overwrite_size != right.overwrite_size ||
        left.native_call.has_value() != right.native_call.has_value()) {
        return false;
    }
    return !left.native_call || equivalent_native_call(*left.native_call, *right.native_call);
}

[[nodiscard]] bool compatible_hook(const HookOperation& request, const InstalledHookSite& installed) noexcept {
    if (request.kind != installed.kind || request.overwrite_size != installed.overwrite_size ||
        request.native_call.has_value() != installed.native_call.has_value()) {
        return false;
    }
    return !request.native_call || equivalent_native_call(*request.native_call, *installed.native_call);
}

[[nodiscard]] const InstalledHookSite* installed_hook(ValidationBaseline baseline, FC_TargetImage image,
                                                      std::uint32_t rva) noexcept {
    const auto found = std::ranges::find_if(baseline.installed_hook_sites, [&](const InstalledHookSite& site) {
        return site.image == image && site.rva == rva;
    });
    return found == baseline.installed_hook_sites.end() ? nullptr : &*found;
}

// Freeze direct hook defects before removing failures so participants evaluated together observe the same outcome.
void validate_hook_groups(PatchWorkSet& patches, ValidationBaseline baseline) {
    auto groups = collect_hook_groups(patches);
    std::vector<std::pair<catalog::PatchIndex, std::string>> direct_failures;
    const auto mark_group = [&](const HookSiteGroup& group, std::string_view message, bool owners_only = false) {
        for (const auto& participant : group.participants) {
            if (!owners_only || !participant.operation->observer) {
                direct_failures.emplace_back(participant.patch, message);
            }
        }
    };
    for (const auto& group : groups) {
        if (std::ranges::any_of(group.participants, [&](const HookReference& participant) {
                return !compatible_hook(*group.participants.front().operation, *participant.operation);
            })) {
            mark_group(group, "Shared hook participants have incompatible shapes for the physical site");
            continue;
        }
        const auto* installed = installed_hook(baseline, group.image, group.rva);
        if (installed != nullptr && std::ranges::any_of(group.participants, [&](const HookReference& participant) {
                return !compatible_hook(*participant.operation, *installed);
            })) {
            mark_group(group, "Shared hook request is incompatible with the installed site");
            continue;
        }
        const auto owner_count = std::ranges::count_if(group.participants, [](const HookReference& participant) {
            return !participant.operation->observer;
        });
        if (owner_count > (installed != nullptr && installed->has_owner ? 0 : 1)) {
            mark_group(group, "Shared hook site has more than one modifying owner", true);
        }
    }
    // Applying after the complete scan prevents one cleared patch plan from rescuing another shared hook site.
    for (auto& [patch, message] : direct_failures) {
        fail_patch(patches, patch, std::move(message), PatchPhase::Validation, "Aggregate shared hook site");
    }
    prune_unavailable_consumers(patches);

    // Previously installed participants consume capacity first; each new observer then resolves without backtracking.
    groups = collect_hook_groups(patches);
    std::vector<catalog::PatchIndex> capacity_failures;
    for (const auto& group : groups) {
        const auto* installed = installed_hook(baseline, group.image, group.rva);
        std::uint32_t observer_count = installed == nullptr ? 0 : installed->observer_count;
        std::uint32_t state_size = installed == nullptr ? 0 : installed->state_size;
        for (const auto& participant : group.participants) {
            if (!participant.operation->observer) {
                continue;
            }
            const auto candidate_count = observer_count + 1;
            auto candidate_size = state_size;
            const auto alignment = participant.operation->state_alignment;
            if (alignment != 0) {
                candidate_size = static_cast<std::uint32_t>((candidate_size + alignment - 1) & ~(alignment - 1));
            }
            if (candidate_count > kHookObserverCapacity ||
                participant.operation->state_size > kHookStateByteCapacity - candidate_size) {
                capacity_failures.push_back(participant.patch);
                continue;
            }
            observer_count = candidate_count;
            state_size = candidate_size + participant.operation->state_size;
        }
    }
    for (const auto patch : capacity_failures) {
        fail_patch(patches, patch, "Shared hook observer or aligned state capacity is exceeded", PatchPhase::Validation,
                   "Aggregate shared hook site");
    }
    prune_unavailable_consumers(patches);
}

// Converts surviving site groups into the owner/observer layout consumed by physical hook installation.
[[nodiscard]] std::vector<HookAggregatePlan> build_hook_aggregates(PatchWorkSet& patches, ValidationBaseline baseline) {
    std::vector<HookAggregatePlan> result;
    for (const auto& group : collect_hook_groups(patches)) {
        const auto* installed = installed_hook(baseline, group.image, group.rva);
        HookAggregatePlan aggregate{.image = group.image,
                                    .rva = group.rva,
                                    .kind = group.participants.front().operation->kind,
                                    .overwrite_size = group.participants.front().operation->overwrite_size,
                                    .state_size = installed == nullptr ? 0 : installed->state_size};
        for (const auto& participant : group.participants) {
            if (!participant.operation->observer) {
                aggregate.owner = HookParticipant{participant.patch, participant.operation_index};
                continue;
            }
            const auto alignment = participant.operation->state_alignment;
            if (alignment != 0) {
                aggregate.state_size =
                    static_cast<std::uint32_t>((aggregate.state_size + alignment - 1) & ~(alignment - 1));
            }
            aggregate.state_size += participant.operation->state_size;
            aggregate.observers.push_back({participant.patch, participant.operation_index});
        }
        result.push_back(std::move(aggregate));
    }
    return result;
}

// Hook candidates name every patch sharing one physical claim; ordinary candidates contain their single owner.
struct ClaimCandidate {
    MemoryClaim claim;
    std::vector<catalog::PatchIndex> participants;
    bool hook{};
};

[[nodiscard]] bool claim_conflict(const MemoryClaim& left, const MemoryClaim& right) noexcept {
    return claims_overlap(left, right) && (left.access == ClaimAccess::Write || right.access == ClaimAccess::Write);
}

[[nodiscard]] bool installed_hook_claim(const ClaimCandidate& candidate, ValidationBaseline baseline) noexcept {
    if (!candidate.hook) {
        return false;
    }
    return std::ranges::any_of(baseline.installed_hook_sites, [&](const InstalledHookSite& site) {
        return site.image == candidate.claim.image && site.rva == candidate.claim.rva &&
               site.overwrite_size == candidate.claim.size;
    });
}

[[nodiscard]] std::vector<ClaimCandidate> collect_claims(PatchWorkSet& patches,
                                                         std::span<const HookAggregatePlan> aggregates) {
    std::vector<ClaimCandidate> result;
    for (const auto& record : patches.records()) {
        if (record.state != PatchState::Pending) {
            continue;
        }
        for (const auto& claim : record.plan.claims) {
            result.push_back({claim, {record.patch}, false});
        }
    }
    // One aggregate claim represents every compatible participant so unrelated conflicts fail the whole site.
    for (const auto& aggregate : aggregates) {
        const auto representative = aggregate.owner ? aggregate.owner->patch : aggregate.observers.front().patch;
        ClaimCandidate candidate{.claim = {.patch = representative,
                                           .image = aggregate.image,
                                           .rva = aggregate.rva,
                                           .size = aggregate.overwrite_size,
                                           .access = ClaimAccess::Write},
                                 .hook = true};
        if (aggregate.owner) {
            candidate.participants.push_back(aggregate.owner->patch);
        }
        for (const auto& observer : aggregate.observers) {
            candidate.participants.push_back(observer.patch);
        }
        result.push_back(std::move(candidate));
    }
    return result;
}

// Applies immutable installed/blocked claims and simultaneous candidate conflicts without choosing a winner.
void validate_claims(PatchWorkSet& patches, ValidationBaseline baseline,
                     std::span<const HookAggregatePlan> aggregates) {
    const auto claims = collect_claims(patches, aggregates);
    std::vector<bool> failed(patches.records().size());
    const auto mark = [&](const ClaimCandidate& candidate) {
        for (const auto patch : candidate.participants) {
            failed[patch.value] = true;
        }
    };

    // Baselines are immutable: only current candidates can be marked by an installed or retained blocker.
    for (const auto& candidate : claims) {
        for (const auto& baseline_claim : baseline.installed_claims) {
            if (claim_conflict(candidate.claim, baseline_claim) && !installed_hook_claim(candidate, baseline)) {
                mark(candidate);
            }
        }
        for (const auto& baseline_claim : baseline.blocked_claims) {
            if (claim_conflict(candidate.claim, baseline_claim)) {
                mark(candidate);
            }
        }
    }
    // Freeze all pairwise conflict participants before terminalizing any record; there is no winner recalculation.
    for (std::size_t left = 0; left < claims.size(); ++left) {
        for (std::size_t right = left + 1; right < claims.size(); ++right) {
            if (!claim_conflict(claims[left].claim, claims[right].claim)) {
                continue;
            }
            std::vector<catalog::PatchIndex> participants = claims[left].participants;
            participants.insert(participants.end(), claims[right].participants.begin(),
                                claims[right].participants.end());
            std::ranges::sort(participants, {}, &catalog::PatchIndex::value);
            const auto unique = std::ranges::unique(participants);
            participants.erase(unique.begin(), unique.end());
            if (participants.size() == 1 &&
                (claims[left].claim.access == ClaimAccess::Read || claims[right].claim.access == ClaimAccess::Read)) {
                continue;
            }
            mark(claims[left]);
            mark(claims[right]);
        }
    }
    for (std::size_t index = 0; index < failed.size(); ++index) {
        if (failed[index]) {
            fail_patch(patches, catalog::PatchIndex{static_cast<std::uint32_t>(index)},
                       "Patch has a direct claim conflict in the game image", PatchPhase::Validation,
                       "Validate memory claims");
        }
    }
    prune_unavailable_consumers(patches);
}

// Orders surviving patches after providers, using stable patch IDs whenever dependency constraints leave a tie.
[[nodiscard]] std::vector<catalog::PatchIndex> topological_order(PatchWorkSet& patches) {
    // Build edges only among current survivors; terminal providers have already pruned their required consumers.
    std::vector<std::uint32_t> indegree(patches.records().size());
    std::vector<std::vector<catalog::PatchIndex>> consumers(patches.records().size());
    std::size_t survivor_count{};
    for (const auto& record : patches.records()) {
        if (record.state != PatchState::Pending) {
            continue;
        }
        ++survivor_count;
        for (const auto& edge : record.required_edges) {
            if (patches.record(edge.provider).state == PatchState::Pending) {
                ++indegree[record.patch.value];
                consumers[edge.provider.value].push_back(record.patch);
            }
        }
    }
    const auto compare = [&](catalog::PatchIndex left, catalog::PatchIndex right) {
        const auto left_id = catalog::fold_ascii(patches.catalog().patch(left).id);
        const auto right_id = catalog::fold_ascii(patches.catalog().patch(right).id);
        return left_id != right_id ? left_id < right_id
                                   : patches.catalog().patch(left).id < patches.catalog().patch(right).id;
    };
    std::set<catalog::PatchIndex, decltype(compare)> ready(compare);
    for (const auto& record : patches.records()) {
        if (record.state == PatchState::Pending && indegree[record.patch.value] == 0) {
            ready.insert(record.patch);
        }
    }
    std::vector<catalog::PatchIndex> result;
    result.reserve(survivor_count);
    // The ordered frontier supplies the required case-insensitive tie-break by patch ID at every graph level.
    while (!ready.empty()) {
        const auto provider = *ready.begin();
        ready.erase(ready.begin());
        result.push_back(provider);
        for (const auto consumer : consumers[provider.value]) {
            if (--indegree[consumer.value] == 0) {
                ready.insert(consumer);
            }
        }
    }
    // A surviving cycle indicates inconsistent graph state; fail its remaining members and settle dependents again.
    if (result.size() != survivor_count) {
        for (auto& record : patches.records()) {
            if (record.state == PatchState::Pending && indegree[record.patch.value] != 0) {
                fail_patch(patches, record.patch, "A cycle among required ordering edges survived initial resolution",
                           PatchPhase::Validation, "Order installation plan");
            }
        }
        prune_unavailable_consumers(patches);
        return topological_order(patches);
    }
    return result;
}

} // namespace

InstallationPlan build_installation_plan(const targets::RecognizedTarget& target, PatchWorkSet& patches,
                                         ValidationBaseline baseline, CoreLogger logger) {
    // First acquire and own each plugin's requested operations; callback failures may invalidate their consumers.
    collect_patch_plans(target, patches, baseline);
    prune_unavailable_consumers(patches);

    // Validate shared physical sites before general claims so compatible hook participants act as one claimant.
    validate_hook_groups(patches, baseline);
    auto aggregates = build_hook_aggregates(patches, baseline);
    validate_claims(patches, baseline, aggregates);

    // Rebuild aggregates after pruning claims, then freeze an installation sequence that respects dependencies.
    aggregates = build_hook_aggregates(patches, baseline);
    auto order = topological_order(patches);
    for (const auto patch : order) {
        patches.record(patch).state = PatchState::Ready;
    }
    // Report only the settled validation state so retries and pruning passes do not duplicate transient failures.
    for (const auto& record : patches.records()) {
        const auto& definition = patches.catalog().patch(record.patch);
        if (record.state == PatchState::Ready) {
            logger.debug("Patch '{}' is in the Ready state with {} operation(s) and {} memory claim(s)", definition.id,
                         record.plan.operations.size(), record.plan.claims.size());
        } else if (record.state == PatchState::Failed && record.reason) {
            logger.error("Patch '{}' failed validation in '{}': {}", definition.id,
                         record.reason->operation.value_or("Validate patch"), record.reason->message);
        } else if (record.state == PatchState::Skipped && record.reason) {
            logger.warning("Patch '{}' was skipped after validation: {}", definition.id, record.reason->message);
        }
    }
    logger.info("Validated {} patch(es) for installation across {} shared hook site(s)", order.size(),
                aggregates.size());
    return {.hook_aggregates = std::move(aggregates), .installation_order = std::move(order)};
}

} // namespace fc::planning
