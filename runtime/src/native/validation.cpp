#include "plan.hpp"

#include "hook_registry.hpp"
#include "native_address.hpp"
#include "plan_detail.hpp"

#include <Windows.h>

#include <algorithm>
#include <bit>
#include <cstring>
#include <format>
#include <limits>
#include <string>

namespace fc::runtime {

namespace plan_detail {

std::string copy_text(FC_Text text) {
    if (text.size && !text.data) {
        throw Error{"Nonempty native text view has a null data pointer"};
    }
    return text.size ? std::string{text.data, text.size} : std::string{};
}

std::vector<std::byte> copy_bytes(FC_Bytes bytes) {
    if (bytes.size && !bytes.data) {
        throw Error{"Nonempty native byte view has a null data pointer"};
    }

    std::vector<std::byte> result(bytes.size);
    if (bytes.size) {
        std::memcpy(result.data(), bytes.data, bytes.size);
    }
    return result;
}

void range(std::uint64_t size, std::uint32_t alignment, std::uintptr_t address) {
    if (!size || size > kMaximum || !std::has_single_bit(alignment) || address % alignment ||
        size > std::numeric_limits<std::uintptr_t>::max() - address) {
        throw Error{"Native range size, end address or alignment is invalid"};
    }
}

std::size_t extent(const EvidenceRecord &evidence) {
    if (evidence.kind == FC_EVIDENCE_POINTS_TO) {
        return sizeof(void *);
    }
    if (evidence.kind == FC_EVIDENCE_DIRECT_CALL_TO || evidence.kind == FC_EVIDENCE_DIRECT_JUMP_TO) {
        return 5;
    }
    return evidence.bytes.size();
}

std::uint64_t encode_field(std::uint32_t size, bool encoded_signed, std::uint64_t value, bool signed_value, bool address_value,
                           bool relative, std::uintptr_t end, FC_Architecture architecture) {
    const auto bits = size * 8;
    if (address_value && relative) {
        if (size == 4) {
            return static_cast<std::uint32_t>(take(planning::encode_rel32(end, static_cast<std::uintptr_t>(value), architecture)));
        }

        // Check the unsigned distance before forming a negative encoding, including the extra negative endpoint.
        const auto target = static_cast<std::uintptr_t>(value);
        const auto distance = target >= end ? target - end : end - target;
        const auto maximum = bits == 64 ? static_cast<std::uint64_t>(INT64_MAX) : (std::uint64_t{1} << (bits - 1)) - 1;
        if (distance > maximum + (target < end)) {
            throw Error{"Native address displacement does not fit its signed encoding"};
        }
        return target >= end ? distance : std::uint64_t{0} - distance;
    }

    // Author signedness matters even when the decoder uses an unsigned or sign-extending field.
    const auto number = std::bit_cast<std::int64_t>(value);
    if (encoded_signed) {
        const auto maximum = bits == 64 ? INT64_MAX : (std::int64_t{1} << (bits - 1)) - 1;
        const auto minimum = bits == 64 ? INT64_MIN : -(std::int64_t{1} << (bits - 1));
        if ((!signed_value && value > static_cast<std::uint64_t>(maximum)) || (signed_value && (number < minimum || number > maximum))) {
            throw Error{"Native operand value does not fit its signed encoding"};
        }
    } else if ((signed_value && number < 0) || (bits < 64 && value > ((std::uint64_t{1} << bits) - 1))) {
        throw Error{"Native operand value does not fit its unsigned encoding"};
    }
    return value;
}

Decoded decode(std::span<const std::byte> bytes) {
    ZydisDecoder decoder{};
    const auto machine = kArchitecture == FC_ARCH_X64 ? ZYDIS_MACHINE_MODE_LONG_64 : ZYDIS_MACHINE_MODE_LEGACY_32;
    const auto width = kArchitecture == FC_ARCH_X64 ? ZYDIS_STACK_WIDTH_64 : ZYDIS_STACK_WIDTH_32;
    Decoded result;
    if (ZYAN_FAILED(ZydisDecoderInit(&decoder, machine, width)) ||
        ZYAN_FAILED(ZydisDecoderDecodeFull(&decoder, bytes.data(), bytes.size(), &result.instruction, result.operands))) {
        throw Error{"Native instruction cannot be decoded"};
    }
    return result;
}

std::vector<std::byte> instructions(const SubmittedPlan &plan, LocationRecord location, std::size_t minimum, bool exact) {
    const auto available = plan.images().at(location.image).size - location.rva;

    // Allow enough lookahead to finish the last instruction when the requested extent may be rounded up.
    const auto requested = exact ? minimum : std::min<std::size_t>(available, minimum + ZYDIS_MAX_INSTRUCTION_LENGTH);
    auto bytes = take(plan.bytes(location, requested));

    // Decode consecutive instructions until their combined length covers the edit's requested bytes.
    std::size_t size{};
    while (size < minimum) {
        const auto decoded = decode(std::span{bytes}.subspan(size));
        size += decoded.instruction.length;
        if (size > requested) {
            throw Error{"Decoded instructions exceed the requested byte range"};
        }
    }

    // Exact replacement lengths must end at an instruction boundary.
    if (exact && size != minimum) {
        throw Error{"Native edit ends inside an instruction"};
    }
    take(plan.access(location, size, true));
    bytes.resize(size);
    return bytes;
}

std::uintptr_t branch_target(std::span<const std::byte> bytes, std::uintptr_t address, std::uint32_t kind) {
    const auto opcode = kind == FC_BRANCH_CALL ? std::byte{0xe8} : std::byte{0xe9};
    if (bytes.size() != 5 || bytes[0] != opcode) {
        throw Error{"Native redirection requires one five-byte direct CALL or unconditional JMP"};
    }

    // The signed displacement follows the opcode and is relative to the instruction's end.
    std::int32_t displacement{};
    std::memcpy(&displacement, bytes.data() + 1, 4);
    auto result = planning::decode_rel32_target(address + 5, displacement, kArchitecture);
    if (!result) {
        throw Error{"Native branch target is outside this architecture's address range"};
    }
    return *result;
}

} // namespace plan_detail

using namespace plan_detail;

std::expected<LocationRecord, Error> SubmittedPlan::location(FC_Location input) const {
    try {
        if (input.invalid) {
            throw Error{"Native location has an invalid derivation"};
        }

        // Resolve the recognized profile ID, then check both image bounds and absolute address arithmetic.
        const auto profile = copy_text(input.image);
        const auto found = std::ranges::find_if(images_, [&](const auto &image) {
            return detail::equal_id(profile, image.profile);
        });
        if (found == images_.end()) {
            throw Error{"Native location does not name a selected image"};
        }
        if (input.rva >= found->size) {
            throw Error{"Native location is outside its selected image"};
        }
        if (input.rva > std::numeric_limits<std::uintptr_t>::max() - found->base) {
            throw Error{"Native location exceeds this architecture's address range"};
        }

        return LocationRecord{static_cast<std::size_t>(found - images_.begin()), input.rva, copy_text(input.name)};
    } catch (const Error &error) {
        return std::unexpected(error);
    }
}

std::expected<void, Error> SubmittedPlan::access(const LocationRecord &location, std::size_t size, bool code) const {
    try {
        const auto &image = images_.at(location.image);
        if (!size || location.rva > image.size || size > image.size - location.rva) {
            throw Error{"Native range is empty or exceeds the selected image"};
        }

        // Offline bytes need one readable section covering the full range; live bytes use current page permissions.
        if (!image.mapped.empty()) {
            if (!std::ranges::any_of(image.sections, [&](const auto &section) {
                    const auto begin = image.base + location.rva;
                    return (section.characteristics & IMAGE_SCN_MEM_READ) && (!code || (section.characteristics & IMAGE_SCN_MEM_EXECUTE)) &&
                           begin >= section.begin && begin - section.begin <= section.size &&
                           size <= section.size - (begin - section.begin);
                })) {
                throw Error{"Offline native range is not contained in a section with the required permissions"};
            }
        } else {
            take(patching::check_native_access(image.base + location.rva, size, false, code));
        }

        return {};
    } catch (const Error &error) {
        return std::unexpected(error);
    }
}

std::expected<std::vector<std::byte>, Error> SubmittedPlan::bytes(const LocationRecord &location, std::size_t size, bool code) const {
    try {
        take(access(location, size, code));
        const auto &image = images_.at(location.image);

        // Copy from the selected backing storage so callers own the observation used for validation.
        auto result = image.mapped.empty()
                          ? take(patching::read_native_memory(image.base + location.rva, size))
                          : std::vector<std::byte>{image.mapped.begin() + location.rva, image.mapped.begin() + location.rva + size};

        // Use saved hook originals only while the live detour still matches the registry's installed bytes.
        if (hooks_) {
            take(hooks_->original_view(image.base + location.rva, result));
        }

        return result;
    } catch (const Error &error) {
        return std::unexpected(error);
    }
}

std::expected<EvidenceRecord, Error> SubmittedPlan::copy_evidence(FC_Evidence input) const {
    try {
        if (input.kind > FC_EVIDENCE_DIRECT_JUMP_TO) {
            throw Error{"Native evidence kind is unknown"};
        }

        // Own temporary byte views and resolve target locations before retaining the evidence.
        EvidenceRecord result{input.kind, copy_bytes(input.bytes), copy_bytes(input.mask)};
        if (input.kind == FC_EVIDENCE_MASKED_BYTES && result.bytes.size() != result.mask.size()) {
            throw Error{"Masked evidence bytes and mask differ in size"};
        }
        if ((input.kind == FC_EVIDENCE_EXACT_BYTES || input.kind == FC_EVIDENCE_MASKED_BYTES) && result.bytes.empty()) {
            throw Error{"Native byte evidence is empty"};
        }
        if (input.kind >= FC_EVIDENCE_POINTS_TO) {
            result.target = take(location(input.target));
        }

        return result;
    } catch (const Error &error) {
        return std::unexpected(error);
    }
}

std::expected<void, Error> SubmittedPlan::evidence(const LocationRecord &location, const EvidenceRecord &evidence,
                                                   std::span<const std::byte> observed) const {
    try {
        if (evidence.kind == FC_EVIDENCE_NONE) {
            return {};
        }

        // Reuse instruction or snapshot originals and read only evidence extending beyond that observation.
        std::vector<std::byte> copied;
        const auto size = extent(evidence);
        if (observed.size() < size) {
            auto tail = location;
            tail.rva += static_cast<std::uint32_t>(observed.size());
            copied.assign(observed.begin(), observed.end());
            auto extra = take(bytes(tail, size - observed.size()));
            copied.insert(copied.end(), extra.begin(), extra.end());
            observed = copied;
        }

        // Apply the mask to expected and observed bytes so only selected bits participate in the comparison.
        if (evidence.kind == FC_EVIDENCE_EXACT_BYTES || evidence.kind == FC_EVIDENCE_MASKED_BYTES) {
            for (std::size_t i = 0; i < evidence.bytes.size(); ++i) {
                const auto mask = evidence.kind == FC_EVIDENCE_MASKED_BYTES ? evidence.mask[i] : std::byte{0xff};
                if ((observed[i] & mask) != (evidence.bytes[i] & mask)) {
                    throw Error{std::format("Native evidence at {}+{:#x} ({}) differs: expected {:#04x}, observed {:#04x}, mask {:#04x}",
                                            images_[location.image].profile, location.rva + i, location.name,
                                            std::to_integer<unsigned>(evidence.bytes[i]), std::to_integer<unsigned>(observed[i]),
                                            std::to_integer<unsigned>(mask))};
                }
            }
        }

        // Pointer and branch evidence compare absolute targets in the selected images' address space.
        if (evidence.kind >= FC_EVIDENCE_POINTS_TO) {
            const auto &target = *evidence.target;
            const auto expected = images_[target.image].base + target.rva;
            std::uintptr_t actual{};
            if (evidence.kind == FC_EVIDENCE_POINTS_TO) {
                std::memcpy(&actual, observed.data(), sizeof(void *));
            } else {
                const auto opcode = evidence.kind == FC_EVIDENCE_DIRECT_CALL_TO ? std::byte{0xe8} : std::byte{0xe9};
                if (observed.front() != opcode) {
                    throw Error{std::format("Native branch evidence at {}+{:#x} ({}) differs: expected opcode {:#04x}, observed {:#04x}",
                                            images_[location.image].profile, location.rva, location.name, std::to_integer<unsigned>(opcode),
                                            std::to_integer<unsigned>(observed.front()))};
                }
                actual = branch_target(observed.first(extent(evidence)), images_[location.image].base + location.rva,
                                       evidence.kind == FC_EVIDENCE_DIRECT_CALL_TO ? FC_BRANCH_CALL : FC_BRANCH_JUMP);
            }
            if (actual != expected) {
                throw Error{std::format("Native target evidence at {}+{:#x} ({}) differs: expected {:#x}, observed {:#x}",
                                        images_[location.image].profile, location.rva, location.name, expected, actual)};
            }
        }

        return {};
    } catch (const Error &error) {
        return std::unexpected(error);
    }
}

std::expected<AddressRecord, Error> SubmittedPlan::copy_address(FC_Address input) const {
    try {
        if (input.invalid || input.kind > FC_ADDRESS_FUNCTION) {
            throw Error{"Native address has an invalid derivation or kind"};
        }

        AddressRecord result;
        result.kind = input.kind;
        result.offset = input.offset;
        result.function = input.function;

        // Image targets retain checked evidence; allocation targets must refer to this plan's own data resource.
        if (input.kind == FC_ADDRESS_IMAGE) {
            result.location = take(location(input.location));
            result.evidence = take(copy_evidence(input.evidence));
            take(evidence(*result.location, result.evidence));
        } else if (input.kind == FC_ADDRESS_ALLOCATION) {
            const auto found = std::ranges::find_if(resources_, [&](const auto &resource) {
                return resource.get() == input.allocation;
            });
            if (found == resources_.end() || (*found)->kind != ResourceKind::Data || input.offset > (*found)->size) {
                throw Error{"Native address must refer to data allocated by this plan with an offset no greater than its size"};
            }
            result.allocation = found->get();
        } else {
            // Function targets must be executable code in the module identified by the submitting factory.
            take(patching::check_native_access(input.function, 1, false, true));
            HMODULE module{};
            if (!plugin_module_ ||
                !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                    reinterpret_cast<LPCWSTR>(input.function), &module) ||
                module != plugin_module_) {
                throw Error{"Could not confirm that the native function target belongs to the submitting plugin"};
            }
        }

        return result;
    } catch (const Error &error) {
        return std::unexpected(error);
    }
}

std::expected<std::uintptr_t, Error> SubmittedPlan::address(const AddressRecord &target, bool code) const {
    try {
        // Resolve against current evidence and mapping permissions each time an image target is used.
        if (target.kind == FC_ADDRESS_IMAGE) {
            const auto &location = *target.location;
            take(access(location, 1, code));
            take(evidence(location, target.evidence));
            return images_[location.image].base + location.rva;
        }

        // Accepted plugin DLLs stay resident; current executable permissions can still change.
        if (target.kind == FC_ADDRESS_FUNCTION) {
            take(patching::check_native_access(target.function, 1, false, true));
            return target.function;
        }

        // Prepared data addresses allow an offset equal to the allocation size to represent its end.
        if (code) {
            throw Error{"Replacement data cannot be used as an executable branch target"};
        }
        if (!target.allocation->ready || target.offset > target.allocation->size ||
            target.offset > std::numeric_limits<std::uintptr_t>::max() - target.allocation->address) {
            throw Error{"Native allocation is not prepared, its offset exceeds its size, or its address overflows"};
        }
        return target.allocation->address + static_cast<std::uintptr_t>(target.offset);
    } catch (const Error &error) {
        return std::unexpected(error);
    }
}

namespace {

// Finds the declaration owning a claim so its hook scope or call layout can participate in conflict checks.
const Operation *claim_operation(const MemoryClaim &claim) {
    const auto &operations = claim.plan->operations();
    return &operations[claim.operation];
}

// Determines whether overlapping requirements can coexist at the same physical or offline image range.
bool claims_conflict(const MemoryClaim &a, const MemoryClaim &b) {
    // Separate offline buffers remain distinct even when their PE preferred bases coincide.
    if (a.offline_image != b.offline_image || a.begin >= b.end || b.begin >= a.end) {
        return false;
    }
    const bool own = a.plan == b.plan;
    if (own && a.operation == b.operation) {
        return false;
    }

    // Consult declarations because hook and callable compatibility includes scope and call layout.
    const auto *left = claim_operation(a), *right = claim_operation(b);
    const auto *left_hook = std::get_if<HookParticipant>(left);
    const auto *right_hook = std::get_if<HookParticipant>(right);
    const auto *left_call = std::get_if<NativeCallOperation>(left);
    const auto *right_call = std::get_if<NativeCallOperation>(right);
    const auto read_only = [](const MemoryClaim &claim) {
        return claim.kind == ClaimKind::Read || claim.kind == ClaimKind::ContinuingRead || claim.kind == ClaimKind::Callable;
    };
    if (read_only(a) && read_only(b)) {
        return false;
    }

    // Continuing readers share mutable data; snapshot and evidence reads retain exclusive reservations against writers.
    if ((a.kind == ClaimKind::ContinuingRead && b.kind == ClaimKind::ContinuingWrite) ||
        (b.kind == ClaimKind::ContinuingRead && a.kind == ClaimKind::ContinuingWrite)) {
        return false;
    }

    // Shared hook sites need matching extents, scope and call layouts, with at most one modifier.
    if (left_hook && right_hook) {
        return a.begin != b.begin || a.end != b.end || left_hook->value.scope != right_hook->value.scope ||
               (left_hook->value.modifier && right_hook->value.modifier) || (left_hook->call.has_value() != right_hook->call.has_value()) ||
               (left_hook->call && !equivalent_call(*left_hook->call, *right_hook->call));
    }

    // A callable entry can share a function hook only when both describe the same entry and call layout.
    if ((left_call && right_hook) || (right_call && left_hook)) {
        const auto &call = left_call ? *left_call : *right_call;
        const auto &hook = left_hook ? *left_hook : *right_hook;
        return a.begin != b.begin || hook.value.scope != FC_HOOK_FUNCTION || !hook.call || !equivalent_call(call.call, *hook.call);
    }

    // Other operation pairs within one plan may overlap a read or continuing write-access claim.
    if (own && (a.kind == ClaimKind::Read || b.kind == ClaimKind::Read || a.kind == ClaimKind::ContinuingRead ||
                b.kind == ClaimKind::ContinuingRead || a.kind == ClaimKind::ContinuingWrite || b.kind == ClaimKind::ContinuingWrite)) {
        return false;
    }
    return true;
}

// Identifies both conflicting owners and the half-open address range shared by their claims.
Error claim_conflict(const MemoryClaim &a, const MemoryClaim &b) {
    return Error{std::format("Native ranges for '{}' and '{}' overlap at [{:#x}, {:#x})", a.plan->owner_id(), b.plan->owner_id(),
                             std::max(a.begin, b.begin), std::min(a.end, b.end))};
}

} // namespace

std::expected<void, Error> SubmittedPlan::validate() const {
    if (error_) {
        return std::unexpected(*error_);
    }

    // Compare every pair because conflicting declarations may occur within one author's plan.
    for (std::size_t i = 0; i < claims_.size(); ++i) {
        for (std::size_t j = i + 1; j < claims_.size(); ++j) {
            const auto &a = claims_[i];
            const auto &b = claims_[j];
            if (claims_conflict(a, b)) {
                return std::unexpected(claim_conflict(a, b));
            }
        }
    }

    return {};
}

std::expected<void, Error> SubmittedPlan::revalidate() const {
    // Compare saved originals at failure time and report the first changed byte's image, RVA and values.
    const auto verify_original = [&](const LocationRecord &location, const std::vector<std::byte> &original, std::string_view subject) {
        const auto observed = take(bytes(location, original.size(), true));
        const auto mismatch = std::ranges::mismatch(original, observed);
        if (mismatch.in1 != original.end()) {
            const auto offset = static_cast<std::size_t>(mismatch.in1 - original.begin());
            const auto label = location.name.empty() ? std::string{} : std::format(" ({})", location.name);
            throw Error{std::format("{} changed before installation at {}+{:#x}{}: expected {:#04x}, observed {:#04x}", subject,
                                    images_[location.image].profile, location.rva + offset, label, std::to_integer<unsigned>(*mismatch.in1),
                                    std::to_integer<unsigned>(*mismatch.in2))};
        }
    };
    if (error_) {
        return std::unexpected(*error_);
    }

    // Revisit retained evidence and the access requirements specific to each operation before installation.
    for (const auto &operation : operations_) {
        auto checked = std::visit(
            [&](const auto &value) -> std::expected<void, Error> {
                using T = std::decay_t<decltype(value)>;
                try {
                    if constexpr (std::same_as<T, Allocation>) {
                        return {};
                    } else {
                        take(evidence(value.location, value.evidence));

                        // Instruction edits and hooks must still match the complete originals saved during submission.
                        if constexpr (std::same_as<T, HookParticipant>) {
                            verify_original(value.location, value.original, "The original hook bytes");
                        } else if constexpr (std::same_as<T, NativeCallOperation>) {
                            take(access(value.location, 1, true));
                        } else if constexpr (std::same_as<T, ByteWrite>) {
                            if (value.instructions) {
                                verify_original(value.location, value.original, "The original write instructions");
                            } else {
                                take(access(value.location, value.bytes.size()));
                            }
                        } else if constexpr (std::same_as<T, ReadClaim>) {
                            // Allocation-backed access uses its prepared address; image access rechecks live permissions.
                            if (value.resource && value.resource->allocation) {
                                take(patching::check_native_access(value.resource->address, static_cast<std::size_t>(value.size),
                                                                   value.writable));
                            } else {
                                take(access(value.location, static_cast<std::size_t>(value.size), value.mode == FC_READ_FAULT));
                                if (value.writable && images_[value.location.image].mapped.empty()) {
                                    take(patching::check_native_access(images_[value.location.image].base + value.location.rva,
                                                                       static_cast<std::size_t>(value.size), true));
                                }
                            }
                        } else if constexpr (std::same_as<T, BranchEdit>) {
                            verify_original(value.location, value.original, "The original branch instructions");
                            take(address(value.target, true));
                        } else if constexpr (std::same_as<T, AddressFixup>) {
                            take(access(value.location, value.relative ? 4 : sizeof(void *)));
                            take(address(value.target));
                        } else if constexpr (std::same_as<T, OperandEdit>) {
                            verify_original(value.location, value.original, "The original operand instruction");
                            if (value.address_value) {
                                take(address(value.address));
                            }
                        }
                        return {};
                    }
                } catch (const Error &error) {
                    return std::unexpected(error);
                }
            },
            operation);
        if (!checked) {
            return checked;
        }
    }

    return {};
}

std::vector<std::pair<SubmittedPlan *, Error>> validate_plans(std::span<SubmittedPlan *const> ready,
                                                              std::span<const MemoryClaim> installed) {
    std::vector<std::pair<SubmittedPlan *, Error>> failures;

    // Installed claims take priority, followed by accepted plans in the caller's dependency order.
    std::vector<MemoryClaim> accepted(installed.begin(), installed.end());
    for (auto *plan : ready) {
        auto valid = plan->validate();
        if (!valid) {
            failures.emplace_back(plan, valid.error());
            continue;
        }

        // Reject the whole plan on its first incompatible overlap with any earlier accepted requirement.
        bool rejected{};
        for (const auto &claim : plan->claims()) {
            for (const auto &prior : accepted) {
                if (!claims_conflict(claim, prior)) {
                    continue;
                }
                failures.emplace_back(plan, claim_conflict(claim, prior));
                rejected = true;
                break;
            }
            if (rejected) {
                break;
            }
        }

        // Keep accepted claims for the rest of this batch even if later preparation fails elsewhere.
        if (!rejected) {
            accepted.insert(accepted.end(), plan->claims().begin(), plan->claims().end());
        }
    }

    return failures;
}

} // namespace fc::runtime
