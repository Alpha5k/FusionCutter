#include "patch_preparation.hpp"

#include "../planning/evidence_validation.hpp"
#include "../planning/native_address.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

namespace fc::patching {
namespace {

[[nodiscard]] planning::FailureReason prepare_failure(std::string message, std::string operation) {
    return {.message = std::move(message), .phase = planning::PatchPhase::Prepare, .operation = std::move(operation)};
}

[[nodiscard]] const catalog::SupportDefinitionRecord& selected_support(const catalog::Catalog& catalog,
                                                                       catalog::PatchIndex patch) noexcept {
    const auto& definition = catalog.patch(patch);
    return definition.supports[*definition.selected_support];
}

// Converts a reviewed RVA to the process address that native preparation will read or overwrite.
[[nodiscard]] std::expected<std::uintptr_t, planning::FailureReason>
image_address(const targets::ImageView& image, std::uint32_t rva, std::string_view operation) {
    if (rva >= image.info().size || image.info().base > std::numeric_limits<std::uintptr_t>::max() - rva) {
        return std::unexpected(
            prepare_failure("Prepared image address is outside the native image", std::string{operation}));
    }
    return image.info().base + rva;
}

// Intersects every rel32 reference to one allocation so a chosen base can encode all of its symbolic writes.
[[nodiscard]] std::expected<std::optional<NearConstraint>, planning::FailureReason>
allocation_constraint(const planning::SubmittedPlan& plan, FC_DataHandle handle, const targets::ImageView& image,
                      FC_Architecture architecture) {
    if (architecture == FC_ARCH_X86) {
        return std::nullopt;
    }
    auto lower = std::uintptr_t{};
    auto upper = std::numeric_limits<std::uintptr_t>::max();
    bool constrained{};
    for (const auto& record : plan.operations) {
        const auto* write = std::get_if<planning::WriteOperation>(&record.payload);
        if (write != nullptr && write->kind == FC_WRITE_REL32 && write->target.kind == FC_ADDRESS_DATA &&
            write->target.data == handle) {
            const auto next = image.info().base + write->location.rva + 4;
            if (write->target.data_offset > std::numeric_limits<std::uintptr_t>::max()) {
                return std::unexpected(
                    prepare_failure("Symbolic rel32 offset exceeds the native address domain", "Place native data"));
            }
            const auto offset = static_cast<std::uintptr_t>(write->target.data_offset);
            constexpr auto negative_reach = static_cast<std::uintptr_t>(std::numeric_limits<std::int32_t>::max()) + 1;
            constexpr auto positive_reach = static_cast<std::uintptr_t>(std::numeric_limits<std::int32_t>::max());
            const auto target_lower = next > negative_reach ? next - negative_reach : 0;
            const auto target_upper = next <= std::numeric_limits<std::uintptr_t>::max() - positive_reach
                                          ? next + positive_reach
                                          : std::numeric_limits<std::uintptr_t>::max();
            if (offset > target_upper) {
                return std::unexpected(
                    prepare_failure("Symbolic rel32 references have no common native placement", "Place native data"));
            }
            // Translate the reachable interior-address interval back to allocation bases without unsigned wrap.
            const auto reference_lower = target_lower > offset ? target_lower - offset : 0;
            const auto reference_upper = target_upper - offset;
            lower = std::max(lower, reference_lower);
            upper = std::min(upper, reference_upper);
            constrained = true;
            if (lower > upper) {
                return std::unexpected(
                    prepare_failure("Symbolic rel32 references have no common native placement", "Place native data"));
            }
        }
    }
    if (!constrained) {
        return std::nullopt;
    }
    // NearConstraint is symmetric, so use the largest symmetric interval wholly contained in the exact intersection.
    const auto reference = lower + (upper - lower) / 2;
    const auto maximum_distance = std::min(reference - lower, upper - reference);
    return NearConstraint{reference, static_cast<std::size_t>(maximum_distance)};
}

// Resolves a symbolic target from the patch plan after preparation assigns addresses to native storage handles.
[[nodiscard]] std::expected<std::uintptr_t, planning::FailureReason>
resolve_target(const planning::AddressTargetRecord& target, const targets::ImageView& image,
               const PatchTransaction& transaction, std::string_view operation) {
    if (target.kind == FC_ADDRESS_IMAGE) {
        return image_address(image, target.image_rva, operation);
    }
    if (target.kind == FC_ADDRESS_PLUGIN_FUNCTION) {
        return target.plugin_function;
    }
    if (target.kind == FC_ADDRESS_DATA) {
        std::uintptr_t address{};
        std::uint64_t byte_size{};
        if (!transaction.resolve_data(target.data, address, byte_size) || target.data_offset > byte_size ||
            target.data_offset > std::numeric_limits<std::uintptr_t>::max() - address) {
            return std::unexpected(
                prepare_failure("Symbolic native storage could not be resolved", std::string{operation}));
        }
        return address + static_cast<std::uintptr_t>(target.data_offset);
    }
    return std::unexpected(prepare_failure("Prepared address target has an unknown kind", std::string{operation}));
}

// Produces a branch destination within rel32 reach, adding an x64 absolute jump relay when needed.
[[nodiscard]] std::expected<std::uintptr_t, planning::FailureReason>
prepare_branch_target(const planning::AddressTargetRecord& target, const targets::ImageView& image,
                      FC_Architecture architecture, std::uintptr_t next, PatchTransaction& transaction,
                      std::string_view operation) {
    auto destination = resolve_target(target, image, transaction, operation);
    if (!destination) {
        return std::unexpected(destination.error());
    }
    if (planning::rel32_reachable(next, *destination, architecture)) {
        return *destination;
    }
    if (architecture != FC_ARCH_X64 || target.kind != FC_ADDRESS_PLUGIN_FUNCTION) {
        return std::unexpected(
            prepare_failure("Prepared branch target is outside signed rel32 reach", std::string{operation}));
    }

    // The relay is a closed absolute jump; a CALL retains its return address while a JMP preserves tail transfer.
    constexpr std::size_t kRelaySize = 14;
    auto relay = NativeAllocation::create(
        kRelaySize, alignof(std::uintptr_t),
        NearConstraint{next, static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())});
    if (!relay) {
        return std::unexpected(prepare_failure(relay.error(), std::string{operation}));
    }
    std::array<std::byte, kRelaySize> bytes{std::byte{0xff}, std::byte{0x25}};
    std::memcpy(bytes.data() + 6, &*destination, sizeof(*destination));
    if (auto initialized = relay->initialize(bytes); !initialized) {
        return std::unexpected(prepare_failure(initialized.error(), std::string{operation}));
    }
    if (auto executable = relay->make_executable(); !executable) {
        return std::unexpected(prepare_failure(executable.error(), std::string{operation}));
    }
    const auto relay_address = relay->address();
    if (!planning::rel32_reachable(next, relay_address, architecture)) {
        return std::unexpected(
            prepare_failure("Allocated branch relay is outside signed rel32 reach", std::string{operation}));
    }
    transaction.add_relay_allocation(std::move(*relay));
    return relay_address;
}

// Encodes a symbolic address write into the architecture-specific bytes published during the Commit phase.
[[nodiscard]] std::expected<std::vector<std::byte>, planning::FailureReason>
encode_address_write(const planning::WriteOperation& write, const targets::ImageView& image,
                     FC_Architecture architecture, PatchTransaction& transaction, std::string_view operation) {
    const auto source = image.info().base + write.location.rva;
    if (write.kind == FC_WRITE_POINTER) {
        auto destination = resolve_target(write.target, image, transaction, operation);
        if (!destination) {
            return std::unexpected(destination.error());
        }
        const auto size = architecture == FC_ARCH_X86 ? 4U : 8U;
        std::vector<std::byte> result(size);
        if (architecture == FC_ARCH_X86) {
            const auto value = static_cast<std::uint32_t>(*destination);
            std::memcpy(result.data(), &value, sizeof(value));
        } else {
            std::memcpy(result.data(), &*destination, sizeof(*destination));
        }
        return result;
    }

    // Relative operands are measured from the end of the encoded instruction or displacement field.
    const auto extent = write.kind == FC_WRITE_REL32 ? 4U : 5U;
    const auto next = source + extent;
    std::expected<std::uintptr_t, planning::FailureReason> destination =
        write.kind == FC_WRITE_REL32
            ? resolve_target(write.target, image, transaction, operation)
            : prepare_branch_target(write.target, image, architecture, next, transaction, operation);
    if (!destination) {
        return std::unexpected(destination.error());
    }
    const auto displacement = planning::encode_rel32(next, *destination, architecture);
    if (!displacement) {
        return std::unexpected(
            prepare_failure("Prepared relative target is outside signed rel32 reach", std::string{operation}));
    }
    std::vector<std::byte> result(extent);
    if (write.kind == FC_WRITE_CALL) {
        result.front() = std::byte{0xe8};
    } else if (write.kind == FC_WRITE_JUMP) {
        result.front() = std::byte{0xe9};
    }
    const auto displacement_offset = write.kind == FC_WRITE_REL32 ? 0U : 1U;
    std::memcpy(result.data() + displacement_offset, &*displacement, sizeof(*displacement));
    return result;
}

// Replaces a reviewed direct branch while preserving whether it transfers by call or jump.
[[nodiscard]] std::expected<std::vector<std::byte>, planning::FailureReason>
encode_redirect(const planning::RedirectOperation& redirect, const targets::ImageView& image,
                FC_Architecture architecture, PatchTransaction& transaction, std::string_view operation) {
    const auto source = image.info().base + redirect.location.rva;
    auto destination = prepare_branch_target(redirect.target, image, architecture, source + 5, transaction, operation);
    if (!destination) {
        return std::unexpected(destination.error());
    }
    const auto displacement = planning::encode_rel32(source + 5, *destination, architecture);
    if (!displacement) {
        return std::unexpected(
            prepare_failure("Redirect target is outside signed rel32 reach", std::string{operation}));
    }
    std::vector<std::byte> result(5);
    result.front() = redirect.kind == FC_REDIRECT_CALL ? std::byte{0xe8} : std::byte{0xe9};
    std::memcpy(result.data() + 1, &*displacement, sizeof(*displacement));
    return result;
}

[[nodiscard]] std::string operation_name(const planning::LocationRecord& location, std::string_view fallback) {
    if (!location.name.empty()) {
        return location.name;
    }
    if (!location.label.empty()) {
        return location.label;
    }
    return std::string{fallback};
}

// Wraps the shared evidence checker in failure metadata for the Prepare phase used by installation diagnostics.
[[nodiscard]] std::expected<void, planning::FailureReason>
validate_evidence(const targets::ImageView& image, FC_Architecture architecture,
                  const planning::LocationRecord& location, std::string_view operation, FC_TargetImage image_id,
                  std::span<const planning::InstalledHookSite> installed_hooks) {
    if (auto valid = planning::validate_location_evidence(image, architecture, location, image_id, installed_hooks);
        !valid) {
        return std::unexpected(prepare_failure(valid.error(), std::string{operation}));
    }
    return {};
}

// Rechecks the reviewed section policy without inventing byte evidence for operations that did not declare any.
[[nodiscard]] std::expected<void, planning::FailureReason>
validate_location_access(const targets::ImageView& image, const planning::LocationRecord& location,
                         std::uint64_t byte_size, bool require_writable, std::string_view operation) {
    if (byte_size > std::numeric_limits<std::size_t>::max()) {
        return std::unexpected(
            prepare_failure("Operation extent no longer fits the native address space", std::string{operation}));
    }
    const auto size = static_cast<std::size_t>(byte_size);
    const bool accessible =
        location.kind == FC_LOCATION_DATA
            ? require_writable ? image.is_writable({location.rva}, size) : image.is_readable({location.rva}, size)
            : image.is_executable({location.rva}, size);
    if (!accessible) {
        return std::unexpected(prepare_failure("Operation location no longer satisfies its reviewed access policy",
                                               std::string{operation}));
    }
    return {};
}

// Image branch destinations must still reside in executable target memory when native bytes are prepared.
[[nodiscard]] std::expected<void, planning::FailureReason>
validate_branch_target(const targets::ImageView& image, const planning::AddressTargetRecord& target,
                       std::string_view operation) {
    if (target.kind == FC_ADDRESS_IMAGE && !image.is_executable({target.image_rva}, 1)) {
        return std::unexpected(
            prepare_failure("Direct branch image target is no longer executable", std::string{operation}));
    }
    return {};
}

// Revalidates both sides of a redirect, including the original branch destination captured during planning.
[[nodiscard]] std::expected<void, planning::FailureReason>
validate_redirect(const targets::ImageView& image, FC_Architecture architecture,
                  const planning::RedirectOperation& redirect, std::string_view operation) {
    if (auto access = validate_location_access(image, redirect.location, 5, false, operation); !access) {
        return access;
    }
    if (auto target = validate_branch_target(image, redirect.target, operation); !target) {
        return target;
    }
    auto original = planning::validate_direct_branch(image, architecture, redirect.location, redirect.kind);
    if (!original) {
        return std::unexpected(prepare_failure(original.error(), std::string{operation}));
    }
    if (*original != redirect.original_target) {
        return std::unexpected(prepare_failure("Original target of the direct branch changed after the Plan callback",
                                               std::string{operation}));
    }
    return {};
}

} // namespace

std::expected<PatchTransaction, planning::FailureReason>
prepare_patch_transaction(const targets::RecognizedTarget& target, const catalog::Catalog& catalog,
                          const planning::PatchWorkRecord& patch, NativeMemoryWriter writer) {
    // Bind preparation to the exact image selected during admission; a plan cannot redirect itself to another image.
    const auto& support = selected_support(catalog, patch.patch);
    const targets::ImageView* image = target.find(support.image);
    if (image == nullptr) {
        return std::unexpected(prepare_failure("Validated patch target image is unavailable", "Prepare native target"));
    }
    if ((target.architecture() == FC_ARCH_X86) != (sizeof(void*) == 4)) {
        return std::unexpected(
            prepare_failure("Target architecture does not match this native installer", "Prepare native target"));
    }

    PatchTransaction transaction{writer};
    // Materialize storage in the order of planned operations so each handle retains the identity of its symbolic write.
    for (const auto& record : patch.plan.operations) {
        const auto* allocation = std::get_if<planning::DataAllocationOperation>(&record.payload);
        if (allocation == nullptr) {
            continue;
        }
        auto constraint = allocation_constraint(patch.plan, allocation->handle, *image, target.architecture());
        if (!constraint) {
            return std::unexpected(constraint.error());
        }
        auto native = NativeAllocation::create(static_cast<std::size_t>(allocation->byte_size), allocation->alignment,
                                               *constraint);
        if (!native) {
            return std::unexpected(
                prepare_failure(native.error(), allocation->name.empty() ? "Allocate native data" : allocation->name));
        }
        if (auto initialized = native->initialize(allocation->initial_bytes); !initialized) {
            return std::unexpected(prepare_failure(
                initialized.error(), allocation->name.empty() ? "Initialize native data" : allocation->name));
        }
        transaction.add_data_allocation(allocation->handle, std::move(*native));
    }

    // Lower validated operations into final byte sequences only after every symbolic handle has a native address.
    for (const auto& record : patch.plan.operations) {
        auto prepared = std::visit(
            [&](const auto& operation) -> std::expected<void, planning::FailureReason> {
                using Operation = std::remove_cvref_t<decltype(operation)>;
                if constexpr (std::same_as<Operation, planning::WriteOperation>) {
                    auto address = image_address(*image, operation.location.rva, "Prepare native write");
                    if (!address) {
                        return std::unexpected(address.error());
                    }
                    auto bytes = operation.kind == FC_WRITE_BYTES
                                     ? std::expected<std::vector<std::byte>, planning::FailureReason>{operation.bytes}
                                     : encode_address_write(operation, *image, target.architecture(), transaction,
                                                            operation_name(operation.location, "Prepare native write"));
                    if (!bytes) {
                        return std::unexpected(bytes.error());
                    }
                    auto added = transaction.add_write(*address, *bytes, record.index,
                                                       operation_name(operation.location, "Commit native write"));
                    return added ? std::expected<void, planning::FailureReason>{}
                                 : std::unexpected(prepare_failure(added.error(), "Prepare native write"));
                } else if constexpr (std::same_as<Operation, planning::NopOperation>) {
                    auto address = image_address(*image, operation.location.rva, "Prepare NOP");
                    if (!address) {
                        return std::unexpected(address.error());
                    }
                    const std::vector<std::byte> nops(static_cast<std::size_t>(operation.size), std::byte{0x90});
                    auto added = transaction.add_write(*address, nops, record.index,
                                                       operation_name(operation.location, "Commit NOP"));
                    return added ? std::expected<void, planning::FailureReason>{}
                                 : std::unexpected(prepare_failure(added.error(), "Prepare NOP"));
                } else if constexpr (std::same_as<Operation, planning::RedirectOperation>) {
                    auto address = image_address(*image, operation.location.rva, "Prepare redirect");
                    if (!address) {
                        return std::unexpected(address.error());
                    }
                    auto bytes = encode_redirect(operation, *image, target.architecture(), transaction,
                                                 operation_name(operation.location, "Prepare redirect"));
                    if (!bytes) {
                        return std::unexpected(bytes.error());
                    }
                    auto added = transaction.add_write(*address, *bytes, record.index,
                                                       operation_name(operation.location, "Commit redirect"));
                    return added ? std::expected<void, planning::FailureReason>{}
                                 : std::unexpected(prepare_failure(added.error(), "Prepare redirect"));
                } else if constexpr (std::same_as<Operation, planning::HookOperation>) {
                    // HookRegistry contributes the site's disabled physical effect or direct call write separately.
                    return {};
                } else {
                    return {};
                }
            },
            record.payload);
        if (!prepared) {
            return std::unexpected(prepared.error());
        }
    }
    return transaction;
}

std::expected<void, planning::FailureReason>
revalidate_prepare_inputs(const targets::RecognizedTarget& target, const catalog::Catalog& catalog,
                          const planning::PatchWorkRecord& patch,
                          std::span<const planning::InstalledHookSite> installed_hooks) {
    const auto image_id = selected_support(catalog, patch.patch).image;
    const auto* image = target.find(image_id);
    if (image == nullptr) {
        return std::unexpected(
            prepare_failure("Patch image disappeared before the Prepare phase", "Revalidate Prepare inputs"));
    }
    // Requirements and redirects depend on facts from the Plan callback, so reject drift before allocating resources.
    for (const auto& record : patch.plan.operations) {
        if (const auto* require = std::get_if<planning::RequireOperation>(&record.payload)) {
            const auto operation = operation_name(require->location, "Revalidate required address");
            if (auto evidence = validate_evidence(*image, target.architecture(), require->location, operation, image_id,
                                                  installed_hooks);
                !evidence) {
                return evidence;
            }
            if (auto access =
                    validate_location_access(*image, require->location, require->size, require->writable, operation);
                !access) {
                return access;
            }
            if (require->resolved_address != image->info().base + require->location.rva) {
                return std::unexpected(prepare_failure("Required address changed before the Prepare phase", operation));
            }
        }
        if (const auto* redirect = std::get_if<planning::RedirectOperation>(&record.payload)) {
            const auto operation = operation_name(redirect->location, "Revalidate redirect original");
            if (auto evidence = validate_evidence(*image, target.architecture(), redirect->location, operation,
                                                  image_id, installed_hooks);
                !evidence) {
                return evidence;
            }
            if (auto branch = validate_redirect(*image, target.architecture(), *redirect, operation); !branch) {
                return branch;
            }
        }
    }
    return {};
}

std::expected<void, planning::FailureReason>
revalidate_commit_inputs(const targets::RecognizedTarget& target, const catalog::Catalog& catalog,
                         const planning::PatchWorkRecord& patch,
                         std::span<const planning::InstalledHookSite> installed_hooks) {
    const auto image_id = selected_support(catalog, patch.patch).image;
    const auto* image = target.find(image_id);
    if (image == nullptr) {
        return std::unexpected(
            prepare_failure("Patch image disappeared before the Commit phase", "Revalidate transaction"));
    }
    // Recheck every operation that depends on an image immediately before publication; records that only allocate have
    // no image state that can drift between the Prepare and Commit phases.
    for (const auto& record : patch.plan.operations) {
        auto valid = std::visit(
            [&](const auto& operation) -> std::expected<void, planning::FailureReason> {
                using Operation = std::remove_cvref_t<decltype(operation)>;
                if constexpr (std::same_as<Operation, planning::DataAllocationOperation> ||
                              std::same_as<Operation, planning::InterfaceBindingOperation>) {
                    return {};
                } else {
                    const auto name = operation_name(operation.location, "Revalidate transaction");
                    // Derive the encoded extent so access checks cover exactly the bytes the Commit phase may touch.
                    if constexpr (!std::same_as<Operation, planning::RedirectOperation>) {
                        std::uint64_t extent{};
                        bool require_writable{};
                        if constexpr (std::same_as<Operation, planning::RequireOperation>) {
                            extent = operation.size;
                            require_writable = operation.writable;
                        } else if constexpr (std::same_as<Operation, planning::WriteOperation>) {
                            if (operation.kind == FC_WRITE_BYTES) {
                                extent = operation.bytes.size();
                            } else if (operation.kind == FC_WRITE_POINTER) {
                                extent = target.architecture() == FC_ARCH_X86 ? 4U : 8U;
                            } else if (operation.kind == FC_WRITE_REL32) {
                                extent = 4;
                            } else {
                                extent = 5;
                            }
                        } else if constexpr (std::same_as<Operation, planning::NopOperation>) {
                            extent = operation.size;
                        } else {
                            extent = operation.overwrite_size;
                        }
                        if (auto access =
                                validate_location_access(*image, operation.location, extent, require_writable, name);
                            !access) {
                            return access;
                        }
                        if constexpr (std::same_as<Operation, planning::WriteOperation>) {
                            if (operation.kind == FC_WRITE_CALL || operation.kind == FC_WRITE_JUMP) {
                                if (auto target_access = validate_branch_target(*image, operation.target, name);
                                    !target_access) {
                                    return target_access;
                                }
                            }
                        }
                    }
                    if (auto evidence = validate_evidence(*image, target.architecture(), operation.location, name,
                                                          image_id, installed_hooks);
                        !evidence) {
                        return evidence;
                    }
                    if constexpr (std::same_as<Operation, planning::RedirectOperation>) {
                        return validate_redirect(*image, target.architecture(), operation, name);
                    } else {
                        return {};
                    }
                }
            },
            record.payload);
        if (!valid) {
            return valid;
        }
    }
    return {};
}

} // namespace fc::patching
