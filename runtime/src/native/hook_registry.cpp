#include "hook_registry.hpp"
#include "../fatal_boundary.hpp"
#include "patches/patch_api.hpp"
#include "native_address.hpp"

#include <Winsock2.h>
#include <Windows.h>
#include <intrin.h>
#include <safetyhook/inline_hook.hpp>
#include <safetyhook/mid_hook.hpp>
#include <safetyhook/os.hpp>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstring>
#include <new>

#include "hook_site.hpp"

namespace fc::runtime {

using hook_detail::instruction_dispatch;
using hook_detail::typed_dispatch;

namespace {

// Owns a diagnostic message in the runtime error type used by registry failures.
Error failure(std::string message) {
    return {std::move(message)};
}

// Unifies backend and runtime failures with the preparation function's exception boundary.
template <class T, class E> T require(std::expected<T, E> value) {
    if (!value) {
        if constexpr (std::same_as<E, Error>) {
            throw value.error();
        } else {
            throw failure("Could not prepare the native hook");
        }
    }

    if constexpr (!std::is_void_v<T>) {
        return std::move(*value);
    }
}

} // namespace

HookPreparation::HookPreparation() = default;

HookPreparation::~HookPreparation() = default;

HookPreparation::HookPreparation(HookPreparation &&) noexcept = default;

HookPreparation &HookPreparation::operator=(HookPreparation &&) noexcept = default;

HookRegistry::HookRegistry() = default;

HookRegistry::~HookRegistry() = default;

std::vector<std::byte> HookRegistry::original_bytes(std::uintptr_t address) const {
    for (const auto &site : sites_) {
        if (site->address == address) {
            return {site->original.begin(), site->original.end()};
        }
    }
    return {};
}

std::expected<void, Error> HookRegistry::original_view(std::uintptr_t address, std::vector<std::byte> &bytes) const {
    for (const auto &site : sites_) {
        if (address >= site->address + site->original.size() || site->address >= address + bytes.size()) {
            continue;
        }

        // Reconstruct original bytes only while the live detour still matches the registry's installed bytes.
        auto current = patching::read_native_memory(site->address, site->installed.size());
        if (!current || *current != site->installed) {
            return std::unexpected(failure("Could not verify the installed native detour bytes"));
        }

        // Replace only the intersection with this site, preserving other bytes in the caller's buffer.
        const auto begin = std::max(address, site->address);
        const auto end = std::min(address + bytes.size(), site->address + site->original.size());
        std::memcpy(bytes.data() + begin - address, site->original.data() + begin - site->address, end - begin);
    }

    return {};
}

std::expected<HookPreparation, Error> HookRegistry::prepare(SubmittedPlan &plan) {
    try {
        HookPreparation result;
        // Group declarations at the same address so each physical site receives one completed list.
        for (auto &operation : plan.operations_) {
            if (auto *hook = std::get_if<HookParticipant>(&operation)) {
                const auto address = plan.images()[hook->location.image].base + hook->location.rva;
                auto pending = std::ranges::find_if(result.items_, [&](const auto &item) {
                    return item.site->address == address;
                });

                if (pending == result.items_.end()) {
                    // Reuse installed sites; new sites and their bridges remain owned by this preparation.
                    HookPreparation::Item item;
                    const auto installed = std::ranges::find_if(sites_, [&](const auto &site) {
                        return site->address == address;
                    });
                    if (installed != sites_.end()) {
                        item.site = installed->get();
                    } else {
                        item.owned = std::make_unique<HookSite>();
                        item.site = item.owned.get();
                        auto &site = *item.site;
                        site.address = address;
                        site.scope = hook->value.scope;
                        site.call = hook->call ? &*hook->call : nullptr;
                        site.original = hook->original;

                        if (site.scope == FC_HOOK_INSTRUCTION) {
                            // A mid-hook entry receives the backend context through the compiler's pointer argument convention.
                            FC_NativeArgument argument{{FC_NATIVE_POINTER, sizeof(void *), alignof(void *)}, {}};
#if defined(_WIN64)
                            argument.storage = {FC_NATIVE_STORAGE_REGISTER, FC_REGISTER_RCX, 0};
                            FC_NativeCall signature{
                                FC_ARCH_X64, FC_CALL_COMPILER, {FC_NATIVE_VOID, 0, 0}, {}, &argument, 1, FC_STACK_CLEANUP_NONE, 0};
#else
                            argument.storage = {FC_NATIVE_STORAGE_STACK, 0, 0};
                            FC_NativeCall signature{
                                FC_ARCH_X86, FC_CALL_COMPILER, {FC_NATIVE_VOID, 0, 0}, {}, &argument, 1, FC_STACK_CLEANUP_CALLER, 4};
#endif

                            site.entry = require(native::ExecutableBridge::entry(signature, &site, instruction_dispatch));
                            site.instruction = require(safetyhook::MidHook::create(
                                reinterpret_cast<void *>(address), reinterpret_cast<safetyhook::MidHookFn>(site.entry.address()),
                                safetyhook::MidHook::StartDisabled));
                        } else {
                            // Typed entries capture logical arguments; the invoker applies the original call's native layout.
                            site.entry = require(native::ExecutableBridge::entry(site.call->view(), &site, typed_dispatch));
                            site.invoker = require(native::ExecutableBridge::invoker(site.call->view()));
                            if (site.scope == FC_HOOK_FUNCTION) {
                                // The disabled function detour supplies a trampoline for displaced original instructions.
                                site.function = require(safetyhook::InlineHook::create(reinterpret_cast<void *>(address),
                                                                                       reinterpret_cast<void *>(site.entry.address()),
                                                                                       safetyhook::InlineHook::StartDisabled));
                                site.original_target = site.function.trampoline().address();
                            } else {
                                // Save the direct CALL's decoded target before replacing its five-byte encoding.
                                std::int32_t relative{};
                                std::memcpy(&relative, site.original.data() + 1, 4);
#if defined(_WIN64)
                                constexpr auto architecture = FC_ARCH_X64;
#else
                                constexpr auto architecture = FC_ARCH_X86;
#endif

                                site.original_target = *planning::decode_rel32_target(address + 5, relative, architecture);
                                auto displacement = planning::encode_rel32(address + 5, site.entry.address(), architecture);
                                auto target = site.entry.address();
                                if (!displacement) {
                                    // A nearby RIP-relative indirect jump reaches an entry outside the CALL's rel32 range.
                                    site.relay = require(patching::create_jump_relay(target, address + 5));
                                    target = site.relay.address();
                                    displacement = planning::encode_rel32(address + 5, target, architecture);
                                }

                                // Keep the final CALL bytes for installation and later external-change checks.
                                site.installed.assign(5, std::byte{});
                                site.installed[0] = std::byte{0xe8};
                                const auto encoded = *displacement;
                                std::memcpy(site.installed.data() + 1, &encoded, 4);
                            }
                        }
                    }

                    // Extend a copy so active invocations keep using their existing immutable list.
                    item.list = std::make_unique<Participants>();
                    if (const auto *previous = item.site->current.load(std::memory_order_acquire)) {
                        item.list->values = previous->values;
                    }
                    result.items_.push_back(std::move(item));
                    pending = std::prev(result.items_.end());
                }

                auto &site = *pending->site;
                assert(site.scope == hook->value.scope);
                assert(!site.call || (hook->call && equivalent_call(*site.call, *hook->call)));
                assert(!hook->value.modifier || !std::ranges::any_of(pending->list->values, [](const auto &participant) {
                    return participant.operation->value.modifier != nullptr;
                }));

                hook->entry = &site.entry;
                hook->invoker = site.invoker.address() ? &site.invoker : nullptr;
                pending->list->values.push_back({&plan, hook});

                if (hook->resource) {
                    // Make the original-call handle ready during preparation, before the physical detour is enabled.
                    hook->resource->address = site.original_target;
                    if (!(site.call->value.flags & FC_CALL_COMPILER)) {
                        hook->resource->borrowed_invoker = &site.invoker;
                    }
                    hook->resource->ready = true;
                }
            }
        }

        for (auto &item : result.items_) {
            // Stable sorting preserves declaration order within each case-insensitive owner ID.
            std::ranges::stable_sort(item.list->values, [](const auto &a, const auto &b) {
                return fc::detail::folded_id(a.plan->owner_id()) < fc::detail::folded_id(b.plan->owner_id());
            });

            // Pack participant state at its required alignment, checking additions before computing offsets.
            for (auto &participant : item.list->values) {
                const auto &operation = participant.operation->value;
                if (operation.modifier) {
                    // Sorting is complete, and the published vector will not grow.
                    item.list->modifier = &participant;
                }
                if (!operation.state_size) {
                    continue;
                }

                const auto alignment = static_cast<std::size_t>(operation.state_alignment);
                if (item.list->state_size > SIZE_MAX - (alignment - 1)) {
                    throw failure("Aligning the native hook's invocation state would overflow the size limit");
                }
                participant.state_offset = (item.list->state_size + alignment - 1) & ~(alignment - 1);
                if (operation.state_size > SIZE_MAX - participant.state_offset) {
                    throw failure("Adding native hook invocation state would overflow the size limit");
                }
                item.list->state_size = participant.state_offset + static_cast<std::size_t>(operation.state_size);
                item.list->state_alignment = std::max(item.list->state_alignment, alignment);
            }

            if (item.owned) {
                // A newly enabled site must already have its list when the first native thread enters.
                item.site->current.store(item.list.get(), std::memory_order_relaxed);
            }
        }

        // Reserve ownership containers before installation can expose native code to these objects.
        sites_.reserve(sites_.size() + result.items_.size());
        for (auto &item : result.items_) {
            item.site->lists.reserve(item.site->lists.size() + 1);
        }

        return result;
    } catch (const Error &error) {
        return std::unexpected(error);
    } catch (const std::exception &error) {
        return std::unexpected(failure(error.what()));
    }
}

std::expected<void, patching::NativeWriteFailure> HookRegistry::install(HookPreparation &preparation, SubmittedPlan &plan) {
    for (auto &item : preparation.items_) {
        auto &site = *item.site;
        if (item.owned) {
            // Activate only new detours; existing sites just receive another published participant list.
            bool enabled{};
            if (site.scope == FC_HOOK_FUNCTION) {
                enabled = site.function.enable().has_value();
            } else if (site.scope == FC_HOOK_INSTRUCTION) {
                enabled = site.instruction.enable().has_value();
            } else {
                // Trap threads around the five-byte CALL while replacing its displacement.
                std::expected<void, patching::NativeWriteFailure> wrote;
                safetyhook::trap_threads(reinterpret_cast<std::uint8_t *>(site.address),
                                         reinterpret_cast<std::uint8_t *>(site.original_target), 5, [&] {
                                             wrote = patching::write_native_memory(site.address, site.installed);
                                         });
                if (!wrote) {
                    // Preserve exposure from this write or any earlier operation in the plan.
                    auto failure = std::move(wrote.error());
                    if (failure.unsafe) {
                        plan.mark_exposed();
                    }
                    failure.unsafe |= plan.exposed();
                    return std::unexpected(std::move(failure));
                }
                enabled = true;
            }

            if (!enabled) {
                plan.mark_exposed();
                return std::unexpected(patching::NativeWriteFailure{"Could not enable the native hook", true});
            }

            if (site.installed.empty()) {
                // Backend detours choose their encoding; retain the actual installed bytes for later checks.
                auto bytes = patching::read_native_memory(site.address, site.original.size());
                if (!bytes) {
                    plan.mark_exposed();
                    return std::unexpected(patching::NativeWriteFailure{bytes.error(), true});
                }
                site.installed = std::move(*bytes);
            }

            sites_.push_back(std::move(item.owned));
            plan.mark_exposed();
        }

        const auto *published = item.list.get();

        // Retain every prior list before publishing the new one; active invocations may still use older participants.
        site.lists.push_back(std::move(item.list));
        // Publishing callbacks at an existing site also makes the plan reachable from native code.
        plan.mark_exposed();
        site.current.store(published, std::memory_order_release);
    }

    return {};
}

} // namespace fc::runtime
