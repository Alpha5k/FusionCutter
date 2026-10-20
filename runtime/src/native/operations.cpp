#include "plan.hpp"

#include "hook_registry.hpp"
#include "plan_detail.hpp"

#include <Winsock2.h>
#include <Windows.h>

#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>

namespace fc::runtime {

using namespace plan_detail;

NativeResource *SubmittedPlan::resource(ResourceKind kind) {
    // Separate allocation keeps opaque handles valid when the owning vector grows.
    auto resource = std::make_unique<NativeResource>();
    resource->kind = kind;
    resource->plan = this;
    auto *pointer = resource.get();
    resources_.push_back(std::move(resource));
    return pointer;
}

std::expected<void, Error> SubmittedPlan::submit(const FC_Operation &input, void **out) {
    if (out) {
        *out = nullptr;
    }
    if (error_) {
        return std::unexpected(*error_);
    }

    try {
        if (preparing_) {
            throw Error{"Native requests must be submitted during Plan"};
        }

        // Associate every range reserved by this request with its eventual operation record.
        const auto index = operations_.size();
        auto add_claim = [&](const LocationRecord &location, std::uint64_t size, ClaimKind kind) {
            const auto address = images_[location.image].base + location.rva;
            range(size, 1, address);
            if (size > images_[location.image].size - location.rva) {
                throw Error{"Native claim exceeds its selected image"};
            }
            claims_.push_back(
                {this, address, address + static_cast<std::uintptr_t>(size), kind, index, images_[location.image].mapped.data()});
        };

        // Reserve only evidence beyond an operation, retaining callable entry anchors for hook compatibility.
        const auto evidence_claim = [&](const LocationRecord &location, const EvidenceRecord &evidence, std::uint64_t size,
                                        ClaimKind kind) {
            if (extent(evidence) > size) {
                auto tail = location;
                const auto offset = kind == ClaimKind::Callable ? 0 : size;
                tail.rva += static_cast<std::uint32_t>(offset);
                add_claim(tail, extent(evidence) - offset, ClaimKind::Read);
            }
        };

        // Check bounds, permissions and evidence, reusing any decoded instruction observation.
        auto checked = [&](FC_Location raw, FC_Evidence raw_evidence, std::uint64_t size, std::uint32_t alignment, ClaimKind kind,
                           bool code = false, std::span<const std::byte> observed = {}) {
            auto location = take(this->location(raw));
            range(size, alignment, images_[location.image].base + location.rva);
            if (observed.empty()) {
                take(access(location, static_cast<std::size_t>(size), code));
            }
            auto evidence = take(copy_evidence(raw_evidence));
            take(this->evidence(location, evidence, observed));
            add_claim(location, size, kind);
            evidence_claim(location, evidence, size, kind);
            return std::pair{std::move(location), std::move(evidence)};
        };

        NativeResource *produced{};

        // Target evidence also reserves its bytes even when the edit occurs in a different image.
        auto claimed_address = [&](FC_Address raw) {
            auto result = take(copy_address(raw));
            if (result.location && extent(result.evidence)) {
                add_claim(*result.location, extent(result.evidence), ClaimKind::Read);
            }
            return result;
        };

        switch (input.kind) {
        case FC_OPERATION_READ: {
            const auto &read = input.value.read;
            if (read.mode == FC_READ_FAILURE) {
                throw Error{copy_text(read.failure)};
            }
            if ((read.mode != FC_READ_MEMORY && read.mode != FC_READ_FAULT) || read.writable > 1) {
                throw Error{"Native read claim has an invalid mode or write permission"};
            }

            // Continuing allocation access shares the data resource whose address preparation will fill.
            if (read.allocation) {
                const auto found = std::ranges::find_if(resources_, [&](const auto &r) {
                    return r.get() == read.allocation;
                });
                if (read.mode != FC_READ_MEMORY || found == resources_.end() || (*found)->kind != ResourceKind::Data) {
                    throw Error{"Continuing allocation access requires memory mode and an allocation owned by this plan"};
                }
                auto *allocation = found->get();
                produced = resource(ResourceKind::Memory);
                produced->allocation = allocation;
                produced->size = allocation->size;
                produced->writable = read.writable != 0;
                operations_.push_back(ReadClaim{{}, {}, produced->size, read.mode, produced->writable, produced});
                break;
            }

            // Fault sites require executable bytes; writable memory also needs current write permission.
            const bool fault = read.mode == FC_READ_FAULT;
            auto [location, evidence] = checked(read.location, read.evidence, read.size, read.alignment,
                                                fault           ? ClaimKind::Read
                                                : read.writable ? ClaimKind::ContinuingWrite
                                                                : ClaimKind::ContinuingRead,
                                                fault);
            const auto address = images_[location.image].base + location.rva;
            if (read.writable && images_[location.image].mapped.empty()) {
                take(patching::check_native_access(address, static_cast<std::size_t>(read.size), true));
            }

            // Reserve the complete faulting instruction, which can extend beyond the declared access size.
            if (fault) {
                if (read.fault_kind < FC_FAULT_READ || read.fault_kind > FC_FAULT_EXECUTE) {
                    throw Error{"Expected native fault has an invalid access kind"};
                }
                auto original = instructions(*this, location, 1, false);
                add_claim(location, original.size(), ClaimKind::Read);
                produced = resource(ResourceKind::Fault);
                produced->fault_kind = read.fault_kind;
            } else if (read.mode == FC_READ_MEMORY) {
                produced = resource(ResourceKind::Memory);
            }

            if (produced) {
                produced->address = address;
                produced->size = read.size;
                produced->writable = read.writable != 0;
            }

            operations_.push_back(ReadClaim{std::move(location), std::move(evidence), read.size, read.mode, read.writable != 0, produced});
            break;
        }

        case FC_OPERATION_WRITE: {
            const auto &write = input.value.write;
            auto replacement = copy_bytes(write.bytes);
            const auto size = write.nop ? write.instruction_bytes : replacement.size();
            // Instruction edits retain one observation for boundary decoding and evidence checks.
            auto original = write.instructions || write.nop
                                ? instructions(*this, take(this->location(write.location)), static_cast<std::size_t>(size), true)
                                : std::vector<std::byte>{};
            auto [location, evidence] =
                checked(write.location, write.evidence, size, write.alignment, ClaimKind::Write, write.instructions || write.nop, original);
            if (write.nop) {
                replacement.assign(static_cast<std::size_t>(size), std::byte{0x90});
            }
            operations_.push_back(ByteWrite{std::move(location), std::move(evidence), std::move(replacement), std::move(original),
                                            write.instructions || write.nop});
            break;
        }

        case FC_OPERATION_OPERAND: {
            const auto &edit = input.value.operand;

            // Decode the complete instruction before selecting an operand's physical encoding.
            auto location = take(this->location(edit.location));
            const auto available = std::min<std::size_t>(ZYDIS_MAX_INSTRUCTION_LENGTH, images_[location.image].size - location.rva);
            auto original = take(bytes(location, available));
            const auto decoded = decode(original);
            original.resize(decoded.instruction.length);
            take(access(location, original.size(), true));
            if (edit.operand >= decoded.instruction.operand_count_visible) {
                throw Error{"Selected explicit operand index is outside the decoded instruction"};
            }

            // Convert Zydis's field widths from bits to bytes while retaining its byte offsets.
            const auto &operand = decoded.operands[edit.operand];
            std::uint32_t offset{}, size{};
            bool encoded_signed{}, relative{};
            if (operand.type == ZYDIS_OPERAND_TYPE_IMMEDIATE) {
                // Count earlier immediate operands to select the matching raw encoding slot.
                std::size_t immediate{};
                for (std::uint32_t i = 0; i < edit.operand; ++i) {
                    if (decoded.operands[i].type == ZYDIS_OPERAND_TYPE_IMMEDIATE) {
                        ++immediate;
                    }
                }
                if (immediate >= 2) {
                    throw Error{"Native immediate encoding is unsupported"};
                }
                const auto &raw = decoded.instruction.raw.imm[immediate];
                offset = raw.offset;
                size = raw.size / 8;
                encoded_signed = raw.is_signed;
                relative = raw.is_relative;
            } else if (operand.type == ZYDIS_OPERAND_TYPE_MEMORY && operand.mem.disp.has_displacement) {
                offset = decoded.instruction.raw.disp.offset;
                size = decoded.instruction.raw.disp.size / 8;
                encoded_signed = true;
                relative = operand.mem.base == ZYDIS_REGISTER_RIP || operand.mem.base == ZYDIS_REGISTER_EIP;
            } else {
                throw Error{"Selected native operand has no supported immediate or displacement field"};
            }

            if (!size || size > 8 || offset + size > original.size()) {
                throw Error{"Native operand field must contain 1 to 8 bytes within the instruction"};
            }

            // Reserve the whole instruction and any additional evidence bytes.
            auto evidence = take(copy_evidence(edit.evidence));
            take(this->evidence(location, evidence, original));
            add_claim(location, original.size(), ClaimKind::Write);
            evidence_claim(location, evidence, original.size(), ClaimKind::Write);
            OperandEdit operation{std::move(location),
                                  std::move(evidence),
                                  edit.address_value != 0,
                                  {},
                                  {},
                                  std::move(original),
                                  offset,
                                  size,
                                  encoded_signed,
                                  relative};
            if (edit.address_value) {
                operation.address = claimed_address(edit.address);
            }

            // Fixed values are encoded during Plan; only allocation placement waits for preparation.
            if (!edit.address_value || operation.address.kind != FC_ADDRESS_ALLOCATION) {
                const auto value = edit.address_value ? take(address(operation.address)) : edit.value;
                const auto end = images_[operation.location.image].base + operation.location.rva + operation.original.size();
                operation.encoded = encode_field(size, encoded_signed, value, edit.signed_value != 0, edit.address_value != 0, relative,
                                                 end, kArchitecture);
            }

            operations_.push_back(std::move(operation));
            break;
        }

        case FC_OPERATION_ALLOCATE: {
            const auto &allocation = input.value.allocation;
            if (!allocation.count || !allocation.element_size || allocation.count > kMaximum / allocation.element_size ||
                !std::has_single_bit(allocation.alignment)) {
                throw Error{"Native allocation size or alignment is invalid"};
            }

            const auto size = allocation.count * allocation.element_size;
            auto initial = copy_bytes(allocation.initial);
            if (!initial.empty() && initial.size() != size) {
                throw Error{"Native allocation initialization must match the allocation's size in bytes"};
            }

            // Resolve the optional placement constraint now; actual storage is allocated during preparation.
            std::optional<std::size_t> near_index;
            const auto near_id = copy_text(allocation.near_image);
            if (!near_id.empty()) {
                const auto found = std::ranges::find_if(images_, [&](const auto &image) {
                    return detail::equal_id(near_id, image.profile);
                });
                if (found == images_.end()) {
                    throw Error{"Near allocation does not name a selected image"};
                }
                near_index = static_cast<std::size_t>(found - images_.begin());
            }

            produced = resource(ResourceKind::Data);
            produced->size = size;
            produced->count = allocation.count;
            produced->writable = true;
            operations_.push_back(Allocation{produced, allocation.alignment, near_index, std::move(initial)});
            break;
        }

        case FC_OPERATION_FIXUP: {
            const auto &fixup = input.value.fixup;
            if ((fixup.relative && fixup.width != 4) || (!fixup.relative && fixup.width != sizeof(void *))) {
                throw Error{"Native address field has an incompatible width"};
            }

            // The public width remains validated; the internal width follows the mode and architecture.
            const bool relative = fixup.relative != 0;
            const auto width = relative ? 4u : static_cast<std::uint32_t>(sizeof(void *));
            auto [location, evidence] = checked(fixup.location, fixup.evidence, width, 1, ClaimKind::Write);
            AddressFixup operation{std::move(location), std::move(evidence), claimed_address(fixup.target), relative, {}};
            if (operation.target.kind != FC_ADDRESS_ALLOCATION) {
                const auto end = images_[operation.location.image].base + operation.location.rva + width;
                operation.encoded =
                    encode_field(width, relative, take(address(operation.target)), false, true, relative, end, kArchitecture);
            }
            operations_.push_back(std::move(operation));
            break;
        }

        case FC_OPERATION_BRANCH: {
            const auto &branch = input.value.branch;
            if (branch.kind > FC_BRANCH_JUMP || branch.insert > 1 ||
                (branch.insert && (branch.instruction_bytes < 5 || branch.original_call))) {
                throw Error{"Native branch kind, insertion flag, instruction range or requested original call is invalid"};
            }

            // Redirects require one direct branch; inserted branches replace a complete author-sized range.
            const auto size = branch.insert ? branch.instruction_bytes : 5;
            auto original = instructions(*this, take(this->location(branch.location)), static_cast<std::size_t>(size), true);
            auto [location, evidence] = checked(branch.location, branch.evidence, size, 1, ClaimKind::Write, true, original);
            auto target = claimed_address(branch.target);
            if (target.kind == FC_ADDRESS_ALLOCATION) {
                throw Error{"Replacement data cannot be used as an executable branch target"};
            }
            take(address(target, true));
            const auto previous = branch.insert ? 0 : branch_target(original, images_[location.image].base + location.rva, branch.kind);

            // Retain a callable original only when the request supplies its native call description.
            std::optional<CallDescription> call;
            if (branch.original_call) {
                call = take(validate_call(*branch.original_call, !(branch.original_call->flags & FC_CALL_COMPILER)));
                if (images_[location.image].mapped.empty()) {
                    take(patching::check_native_access(previous, 1, false, true));
                }
                produced = resource(ResourceKind::Call);
                produced->address = previous;
            }

            operations_.push_back(BranchEdit{std::move(location), std::move(evidence), std::move(target), branch.kind, std::move(original),
                                             previous, produced, std::move(call)});
            break;
        }

        case FC_OPERATION_HOOK: {
            const auto &hook = input.value.hook;
            if (hook.scope > FC_HOOK_INSTRUCTION || (!hook.modifier && !hook.before && !hook.after) ||
                (hook.modifier && (hook.before || hook.after)) ||
                (hook.state_size && (!hook.initialize || !std::has_single_bit(hook.state_alignment))) || hook.state_size > kMaximum) {
                throw Error{"Native hook scope, callbacks or state layout are invalid"};
            }

            // Executable callback pointers must belong to the submitting plugin when its module is known.
            const auto callback = [&](std::uintptr_t function) {
                if (!function) {
                    return;
                }
                take(patching::check_native_access(function, 1, false, true));
                if (plugin_module_) {
                    HMODULE module{};
                    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                            reinterpret_cast<LPCWSTR>(function), &module) ||
                        module != plugin_module_) {
                        throw Error{"Could not verify that the native hook callback belongs to the submitting plugin"};
                    }
                }
            };

            callback(reinterpret_cast<std::uintptr_t>(hook.modifier));
            callback(reinterpret_cast<std::uintptr_t>(hook.before));
            callback(reinterpret_cast<std::uintptr_t>(hook.after));
            callback(reinterpret_cast<std::uintptr_t>(hook.initialize));

            // A shared installed hook contributes its saved original instructions to new declarations.
            auto location = take(this->location(hook.location));
            std::vector<std::byte> original;
            if (hooks_) {
                original = hooks_->original_bytes(images_[location.image].base + location.rva);
            }
            if (!original.empty()) {
                original = take(bytes(location, original.size(), true));
            }
            if (original.empty()) {
                original = instructions(*this, location, 5, hook.scope == FC_HOOK_CALL);
            }
            if (hook.scope == FC_HOOK_CALL) {
                branch_target(original, images_[location.image].base + location.rva, FC_BRANCH_CALL);
            }

            // Typed hooks validate their call layout; instruction hooks operate on a CPU context.
            auto evidence = take(copy_evidence(hook.evidence));
            take(this->evidence(location, evidence, original));
            std::optional<CallDescription> call;
            if (hook.scope != FC_HOOK_INSTRUCTION) {
                if (!hook.call) {
                    throw Error{"Typed native hook has no call description"};
                }
                call = take(validate_call(*hook.call, true));
                if (hook.modifier) {
                    produced = resource(ResourceKind::Call);
                }
            } else if (hook.call) {
                throw Error{"Instruction hook cannot have a typed call description"};
            }

            // Keep trailing evidence as a separate read range so compatible hooks can share the displaced bytes.
            add_claim(location, original.size(), ClaimKind::Hook);
            if (extent(evidence) > original.size()) {
                auto tail = location;
                tail.rva += static_cast<std::uint32_t>(original.size());
                add_claim(tail, extent(evidence) - original.size(), ClaimKind::Read);
            }

            // Own the location, evidence and call records; callback and context pointers remain borrowed.
            auto copied = hook;
            copied.location = {};
            copied.evidence = {};
            copied.call = nullptr;
            operations_.push_back(
                HookParticipant{std::move(location), std::move(evidence), copied, std::move(call), std::move(original), produced});
            break;
        }

        case FC_OPERATION_CALL: {
            // Callable entries reserve their executable byte and any wider evidence used to identify them.
            const auto &call = input.value.call;
            auto [location, evidence] = checked(call.location, call.evidence, 1, 1, ClaimKind::Callable, true);
            produced = resource(ResourceKind::Call);
            produced->address = images_[location.image].base + location.rva;
            operations_.push_back(NativeCallOperation{std::move(location), std::move(evidence),
                                                      take(validate_call(call.call, !(call.call.flags & FC_CALL_COMPILER))), produced});
            break;
        }
        default:
            throw Error{"Native operation tag is unknown"};
        }

        // Return any produced handle after copying succeeds; it becomes usable after successful preparation.
        if (out) {
            *out = produced;
        }
        return {};
    } catch (const Error &error) {
        fail(error);
    } catch (const std::exception &exception) {
        fail({exception.what()});
    } catch (...) {
        fail({"Native request submission failed"});
    }

    // Partial claims and resources can remain after failure; the retained error prevents their preparation or installation.
    return std::unexpected(*error_);
}

std::expected<void, Error> SubmittedPlan::read(const FC_ReadClaim &claim, void *out) {
    if (error_) {
        return std::unexpected(*error_);
    }

    try {
        if (!out || preparing_) {
            throw Error{"Native snapshot requires output storage and submission during Plan"};
        }

        // Read once for both the caller's snapshot and its evidence, including evidence beyond the requested value.
        auto location = take(this->location(claim.location));
        const auto address = images_[location.image].base + location.rva;
        range(claim.size, claim.alignment, address);
        auto evidence = take(copy_evidence(claim.evidence));
        const auto size = std::max(static_cast<std::size_t>(claim.size), extent(evidence));
        auto snapshot = take(bytes(location, size));
        take(this->evidence(location, evidence, snapshot));

        // Reserve all observed bytes before returning just the requested snapshot to the caller.
        claims_.push_back({this, address, address + size, ClaimKind::Read, operations_.size(), images_[location.image].mapped.data()});
        operations_.push_back(ReadClaim{location, std::move(evidence), claim.size, FC_READ_SNAPSHOT, false, nullptr});
        std::memcpy(out, snapshot.data(), static_cast<std::size_t>(claim.size));
        return {};
    } catch (const Error &error) {
        fail(error);
    } catch (const std::exception &exception) {
        fail({exception.what()});
    }

    return std::unexpected(*error_);
}

} // namespace fc::runtime
