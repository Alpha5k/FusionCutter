#include "plan.hpp"

#include "hook_registry.hpp"
#include "native_address.hpp"
#include "plan_detail.hpp"

#include <algorithm>
#include <cstring>
#include <format>
#include <string>

namespace fc::runtime {

using namespace plan_detail;

std::expected<void, Error> SubmittedPlan::prepare() {
    // A failed preparation stays closed to submission; later calls return its retained failure.
    if (preparing_) {
        return error_ ? std::unexpected(*error_) : std::expected<void, Error>{};
    }

    try {
        if (error_) {
            return std::unexpected(*error_);
        }

        // Close submission before allocating resources so preparation uses the complete set of requests.
        preparing_ = true;

        // Allocate and initialize all replacement storage before resolving allocation-backed addresses.
        for (auto &operation : operations_) {
            if (auto *allocation = std::get_if<Allocation>(&operation)) {
                std::optional<patching::NearConstraint> constraint;
                if (allocation->near_image) {
                    constraint = {{images_[*allocation->near_image].base, INT32_MAX}};
                }
                auto &resource = *allocation->resource;
                resource.storage =
                    take(patching::NativeAllocation::create(static_cast<std::size_t>(resource.size), allocation->alignment, constraint));
                take(resource.storage.initialize(allocation->initial));
                resource.address = resource.storage.address();
                resource.ready = true;
            }
        }

        // Retain complete write buffers without changing live image memory during preparation.
        const auto write = [&](const LocationRecord &location, std::vector<std::byte> value) {
            writes_.push_back({images_[location.image].base + location.rva, std::move(value), location});
        };

        // Compiler calls use their C++ signature; custom layouts need a generated frame invoker.
        const auto call = [&](NativeResource &resource, const CallDescription &description, std::uintptr_t target) {
            resource.address = target;
            if (!(description.value.flags & FC_CALL_COMPILER)) {
                resource.invoker = take(native::ExecutableBridge::invoker(description.view()));
            }
            resource.ready = true;
        };

        // Allocation addresses now exist, so resolve references and encode each final replacement.
        for (auto &operation : operations_) {
            std::visit(
                [&](auto &value) {
                    using T = std::decay_t<decltype(value)>;
                    if constexpr (std::same_as<T, ReadClaim>) {
                        if (value.resource) {
                            auto &resource = *value.resource;
                            if (resource.allocation) {
                                resource.address = resource.allocation->address;
                            }
                            resource.ready = true;
                        }
                    } else if constexpr (std::same_as<T, ByteWrite>) {
                        write(value.location, value.bytes);
                    } else if constexpr (std::same_as<T, AddressFixup>) {
                        const auto width = value.relative ? 4u : static_cast<std::uint32_t>(sizeof(void *));
                        auto encoded = value.encoded;
                        if (!encoded) {
                            const auto end = images_[value.location.image].base + value.location.rva + width;
                            encoded = encode_field(width, value.relative, take(address(value.target)), false, true, value.relative, end,
                                                   kArchitecture);
                        }
                        std::vector<std::byte> bytes(width);
                        std::memcpy(bytes.data(), &*encoded, width);
                        write(value.location, std::move(bytes));
                    } else if constexpr (std::same_as<T, OperandEdit>) {
                        auto encoded = value.encoded;
                        if (!encoded) {
                            const auto end = images_[value.location.image].base + value.location.rva + value.original.size();
                            encoded = encode_field(value.field_size, value.encoded_signed, take(address(value.address)), false, true,
                                                   value.relative, end, kArchitecture);
                        }

                        // Preserve other instruction fields and use the already checked encoding bytes.
                        auto bytes = value.original;
                        std::memcpy(bytes.data() + value.field_offset, &*encoded, value.field_size);
                        write(value.location, std::move(bytes));
                    } else if constexpr (std::same_as<T, BranchEdit>) {
                        const auto site = images_[value.location.image].base + value.location.rva;
                        auto target = take(address(value.target, true));
                        auto displacement = planning::encode_rel32(site + 5, target, kArchitecture);

                        // Use a nearby indirect-jump relay when the final target is outside the rel32 range.
                        if (!displacement) {
                            auto relay = take(patching::create_jump_relay(target, site + 5));
                            displacement = planning::encode_rel32(site + 5, relay.address(), kArchitecture);
                            relays_.push_back(std::move(relay));
                        }

                        // Fill the remaining displaced instructions with NOPs after the five-byte branch.
                        auto bytes = std::vector<std::byte>(value.original.size(), std::byte{0x90});
                        bytes[0] = value.kind == FC_BRANCH_CALL ? std::byte{0xe8} : std::byte{0xe9};
                        const auto relative = take(std::move(displacement));
                        std::memcpy(bytes.data() + 1, &relative, 4);
                        write(value.location, std::move(bytes));

                        if (value.resource) {
                            call(*value.resource, *value.call, value.original_target);
                        }
                    } else if constexpr (std::same_as<T, NativeCallOperation>) {
                        call(*value.resource, value.call, value.resource->address);
                    }
                },
                operation);
        }

        // Prepare hook bridges and original-call handles before the author's Prepare callback can use them.
        if (hooks_) {
            hook_preparation_ = std::make_unique<HookPreparation>(take(hooks_->prepare(*this)));
        } else if (std::ranges::any_of(operations_, [](const auto &value) {
                       return std::holds_alternative<HookParticipant>(value);
                   })) {
            throw Error{"Native hook registry is unavailable"};
        }

        return {};
    } catch (const Error &error) {
        // Partial resources remain owned by the plan until its owner discards the failed preparation.
        fail(error);
    } catch (const std::exception &exception) {
        fail({exception.what()});
    }

    return std::unexpected(*error_);
}

std::expected<void, patching::NativeWriteFailure> SubmittedPlan::install() {
    if (std::ranges::any_of(images_, [](const auto &image) {
            return !image.mapped.empty();
        })) {
        return std::unexpected(patching::NativeWriteFailure{"Offline native plans cannot be installed", false});
    }

    // Recheck originals and permissions before the first live write can expose the owner.
    const auto checked = revalidate();
    if (!checked) {
        return std::unexpected(patching::NativeWriteFailure{checked.error().message, false});
    }

    // Apply writes in submission order and retain the owner as soon as a write may have taken effect.
    for (const auto &operation : writes_) {
        auto result = patching::write_native_memory(operation.address, operation.bytes);
        if (!result) {
            auto failure = std::move(result.error());
            if (failure.unsafe) {
                mark_exposed();
            }
            if (exposed_) {
                // Earlier writes make this failure unsafe even if the failing write changed no bytes.
                failure.unsafe = true;
            }
            const auto label = operation.location.name.empty() ? std::string{} : std::format(" ({})", operation.location.name);
            failure.message = std::format("Could not write {}+{:#x}{}: {}", images_[operation.location.image].profile,
                                          operation.location.rva, label, failure.message);
            return std::unexpected(std::move(failure));
        }
        mark_exposed();
    }

    // Ordinary writes precede physical hook installation and publication of prepared callbacks.
    if (hook_preparation_) {
        auto result = hooks_->install(*hook_preparation_, *this);
        if (!result) {
            return result;
        }
    }

    // Close allocation initialization views after successful installation, even for plans without writes or hooks.
    mark_exposed();
    return {};
}

} // namespace fc::runtime
