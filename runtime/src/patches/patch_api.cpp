#include "patch_api.hpp"

#include "patch_record.hpp"
#include "core_runtime.hpp"
#include "fatal_boundary.hpp"

#include <fc/base.hpp>

#include <algorithm>
#include <cstring>
#include <exception>
#include <format>
#include <new>
#include <optional>
#include <stdexcept>
#include <utility>

namespace fc::runtime {

namespace {

// Nested provider calls keep the outermost consumer's identity for fault and log attribution.
thread_local PatchRecord *originating_consumer{};

// Attributes an API failure to the patch that requested the operation.
Diagnostic failure(const PatchRecord &owner, std::string message) {
    return {qualified_patch_id(owner), std::move(message)};
}

// Borrows the plugin and patch IDs that identify one API provider.
struct ProviderReference {
    std::string_view plugin, patch;
};

// Resolves patch or plugin.patch into borrowed IDs; returns nullopt for empty IDs or extra dots.
std::optional<ProviderReference> reference(const PatchRecord &consumer, std::string_view value) {
    // Split plugin.patch references while resolving an unqualified patch within the consumer's plugin.
    const auto dot = value.find('.');
    ProviderReference result{dot == std::string_view::npos ? fc::detail::view(consumer.plugin->admitted.definition->id)
                                                           : value.substr(0, dot),
                             dot == std::string_view::npos ? value : value.substr(dot + 1)};
    if (result.plugin.empty() || result.patch.empty() || result.patch.find('.') != std::string_view::npos) {
        return std::nullopt;
    }
    return result;
}

// Compares provider plugin and patch IDs ASCII case-insensitively.
bool matches(const PatchRecord &patch, const ProviderReference &provider) {
    return fc::detail::equal_id(fc::detail::view(patch.plugin->admitted.definition->id), provider.plugin) &&
           fc::detail::equal_id(fc::detail::view(patch.definition->id), provider.patch);
}

// Returns the first malformed contract condition, or nullopt when storage extents are usable.
std::optional<std::string> invalid_contract(const PatchApiContract &contract) {
    if (contract.id.empty()) {
        return "API contract ID is empty";
    }
    const auto alignment = contract.table_alignment;
    // A power-of-two alignment has one set bit; table sizes must be multiples of that alignment.
    if (contract.table_size == 0 || alignment == 0 || (alignment & (alignment - 1)) != 0 || contract.table_size % alignment != 0) {
        return "API '" + std::string{contract.id} + "' has an invalid table size or alignment";
    }
    if (contract.description.empty()) {
        return "API '" + std::string{contract.id} + "' has no table layout and function signature description";
    }

    // Required offsets locate function pointers; each complete slot must fit within the copied table.
    for (const auto offset : contract.required_functions) {
        if (offset > contract.table_size || sizeof(PatchApiAvailable) > contract.table_size - offset) {
            return "API '" + std::string{contract.id} + "' has a required function pointer that extends beyond its table";
        }
    }

    return std::nullopt;
}

} // namespace

PatchApiReservations::PatchApiReservations(PatchRecord &owner) noexcept : owner_(&owner) {}

void PatchApiReservations::AlignedDelete::operator()(std::byte *memory) const noexcept {
    ::operator delete(memory, std::align_val_t{alignment});
}

PatchApiReservations::Contract PatchApiReservations::copy_contract(const PatchApiContract &source) {
    return {std::string{source.id}, source.table_size, source.table_alignment,
            std::vector<std::byte>{source.description.begin(), source.description.end()},
            std::vector<std::uint32_t>{source.required_functions.begin(), source.required_functions.end()}};
}

std::expected<void, Diagnostic> PatchApiReservations::publish(const PatchApiContract &contract, const void *table) {
    if (owner_->state != PatchState::Preparing) {
        return std::unexpected(failure(*owner_, std::format("Cannot publish API '{}' while the patch is {}; publish during prepare()",
                                                            contract.id, state_name(owner_->state))));
    }
    if (auto invalid = invalid_contract(contract)) {
        return std::unexpected(failure(*owner_, std::move(*invalid)));
    }
    if (std::ranges::any_of(publications_, [&](const auto &item) {
            return item.contract.id == contract.id;
        })) {
        return std::unexpected(failure(*owner_, "API '" + std::string{contract.id} + "' was already published by this patch"));
    }

    // Retain the contract and aligned table before exposing either; vector movement cannot relocate table bytes.
    auto copied = copy_contract(contract);
    const auto alignment = std::max<std::size_t>(contract.table_alignment, __STDCPP_DEFAULT_NEW_ALIGNMENT__);
    std::unique_ptr<std::byte, AlignedDelete> storage{
        static_cast<std::byte *>(::operator new(contract.table_size, std::align_val_t{alignment})), AlignedDelete{alignment}};
    std::memcpy(storage.get(), table, contract.table_size);
    publications_.push_back({std::move(copied), std::move(storage)});
    return {};
}

std::expected<std::span<const std::byte>, Diagnostic> PatchApiReservations::connect(const PatchApiReservations &provider,
                                                                                    const PatchApiContract &contract) const {
    const auto found = std::ranges::find_if(provider.publications_, [&](const auto &item) {
        return item.contract.id == contract.id;
    });
    const auto label = "API '" + std::string{contract.id} + "' from provider '" + qualified_patch_id(*provider.owner_) + "'";
    if (found == provider.publications_.end()) {
        return std::unexpected(failure(*owner_, label + " is not published"));
    }

    const auto &published = found->contract;
    // An API ID alone is insufficient: table layout, signatures and required pointer slots must all match.
    if (published.table_size != contract.table_size || published.table_alignment != contract.table_alignment ||
        !std::ranges::equal(published.description, contract.description) ||
        !std::ranges::equal(published.required_functions, contract.required_functions)) {
        return std::unexpected(failure(*owner_, label + " has an incompatible contract"));
    }

    // Connection checks required function slots after comparing the copied contract descriptions.
    for (const auto offset : published.required_functions) {
        PatchApiAvailable function{};
        std::memcpy(&function, found->table.get() + offset, sizeof(function));
        if (!function) {
            return std::unexpected(failure(*owner_, label + " has a null required function at byte offset " + std::to_string(offset)));
        }
    }

    return std::span<const std::byte>{found->table.get(), published.table_size};
}

std::expected<std::span<const std::byte>, Diagnostic> PatchApiReservations::get(std::string_view provider,
                                                                                const PatchApiContract &contract) const {
    if (owner_->state != PatchState::Preparing) {
        return std::unexpected(
            failure(*owner_, std::format("Cannot retrieve API '{}' from provider '{}' while the patch is {}; retrieve during prepare()",
                                         contract.id, provider, state_name(owner_->state))));
    }
    const auto parsed = reference(*owner_, provider);
    if (!parsed) {
        return std::unexpected(failure(*owner_, "API provider reference '" + std::string{provider} + "' is invalid"));
    }

    // Dependency resolution has already expanded groups; authorize the exact stable provider identity it retained.
    const auto dependency = std::ranges::find_if(owner_->providers, [&](const auto *item) {
        return matches(*item, *parsed);
    });
    if (dependency == owner_->providers.end()) {
        return std::unexpected(
            failure(*owner_,
                    std::format("Cannot retrieve API '{}': provider '{}' is not covered by a declared dependency", contract.id, provider)));
    }
    if ((*dependency)->state != PatchState::Installed) {
        return std::unexpected(
            failure(*owner_, std::format("Cannot retrieve API '{}': provider '{}' is {}; the provider must be installed and activated",
                                         contract.id, provider, state_name((*dependency)->state))));
    }
    return connect(*(*dependency)->api, contract);
}

std::expected<void, Diagnostic> PatchApiReservations::on_available(std::string_view provider, const PatchApiContract &contract,
                                                                   void *context, PatchApiAvailable callback,
                                                                   const SelectionSession &session) {
    if (owner_->state != PatchState::Preparing) {
        return std::unexpected(failure(
            *owner_,
            std::format(
                "Cannot register a callback for optional API '{}' from provider '{}' while the patch is {}; register during prepare()",
                contract.id, provider, state_name(owner_->state))));
    }
    if (auto invalid = invalid_contract(contract)) {
        return std::unexpected(failure(*owner_, std::move(*invalid)));
    }
    const auto parsed = reference(*owner_, provider);
    if (!parsed || !callback) {
        return std::unexpected(failure(*owner_, "Optional API '" + std::string{contract.id} + "' requires a valid provider and callback"));
    }

    // Only selected records can become available; retain waiting providers until their later activation.
    const auto found = std::ranges::find_if(session.patches, [&](const auto &record) {
        return matches(*record, *parsed);
    });
    if (found != session.patches.end()) {
        subscriptions_.push_back({found->get(), copy_contract(contract), context, callback});
    }
    return {};
}

void PatchApiReservations::deliver_available(std::vector<Diagnostic> &diagnostics) {
    if (owner_->state != PatchState::Installed) {
        return;
    }

    for (auto &subscription : subscriptions_) {
        auto *provider = subscription.provider;
        if (!provider || provider->state != PatchState::Installed) {
            continue;
        }

        // An active provider resolves this optional subscription to one table or one diagnostic.
        const auto &saved = subscription.contract;
        const PatchApiContract contract{saved.id, saved.table_size, saved.table_alignment, saved.description, saved.required_functions};
        auto table = connect(*provider->api, contract);
        if (!table) {
            table.error().level = FC_LOG_WARNING;
            table.error().message += "; the optional API callback will not run";
            diagnostics.push_back(std::move(table.error()));
            subscription.provider = nullptr;
            continue;
        }

        // Clear the pending provider before author code can reenter delivery.
        subscription.provider = nullptr;
        fc::invoke_patch_callback([&] {
            subscription.callback(subscription.context, table->data());
        });
    }
}

void PatchApiReservations::discard() noexcept {
    subscriptions_.clear();
    publications_.clear();
}

void PatchApiReservations::fail(Diagnostic diagnostic) {
    if (!error_) {
        error_ = std::move(diagnostic);
    }
}

PatchRecord *current_consumer() noexcept {
    return originating_consumer;
}

PatchRecord *enter_patch_callback(PatchRecord *owner) noexcept {
    auto *previous = originating_consumer;
    // A nested callback inherits the consumer that started the synchronous chain.
    if (!previous) {
        originating_consumer = owner;
    }
    return previous;
}

void leave_patch_callback(PatchRecord *previous) noexcept {
    originating_consumer = previous;
}

namespace {

// Borrows ABI contract storage for this operation; throws when a nonzero count has no storage.
PatchApiContract contract_view(const FC_ApiContract &contract) {
    // Reject null counted storage before constructing spans over caller-supplied ABI data.
    if ((contract.id.size && !contract.id.data) || (contract.description.size && !contract.description.data) ||
        (contract.function_count && !contract.required_functions)) {
        throw std::invalid_argument("Patch API contract has a null data pointer for a nonempty field");
    }
    return {fc::detail::view(contract.id),
            contract.table_size,
            contract.table_alignment,
            {reinterpret_cast<const std::byte *>(contract.description.data), contract.description.size},
            {contract.required_functions, contract.function_count}};
}

// Records the patch's first API failure and sends that retained error to the optional synchronous sink.
void report_api_error(PatchRecord &owner, Diagnostic diagnostic, FC_ErrorSink sink) {
    // Retain the first failure for installation even if the author ignores this operation's return value.
    owner.api->fail(std::move(diagnostic));
    const auto &error = *owner.api->error();
    if (sink.write) {
        sink.write(sink.context, {error.message.data(), static_cast<std::uint32_t>(error.message.size())}, error.windows_error);
    }
}

// Converts reservation failures and C++ exceptions to ABI results and the patch's first Prepare error.
template <class F> std::uint32_t api_operation(void *identity, FC_ErrorSink error, F &&operation) {
    auto *owner = static_cast<PatchRecord *>(identity);
    if (!owner || !owner->api || !owner->runtime) {
        if (error.write) {
            constexpr std::string_view message = "Patch API owner is unavailable";
            error.write(error.context, {message.data(), static_cast<std::uint32_t>(message.size())}, 0);
        }
        return 0;
    }

    try {
        auto result = operation(*owner);
        if (result) {
            return 1;
        }
        report_api_error(*owner, std::move(result.error()), error);
    } catch (const std::exception &exception) {
        report_api_error(*owner, failure(*owner, exception.what()), error);
    } catch (...) {
        report_api_error(*owner, failure(*owner, "Patch API operation raised an unknown exception"), error);
    }

    return 0;
}

// Copies the ABI contract and table into this preparing patch's reservations; returns 1 on success.
std::uint32_t FC_CDECL publish_api(void *owner, const FC_ApiContract *contract, const void *table, FC_ErrorSink error) {
    return api_operation(owner, error, [&](PatchRecord &record) -> std::expected<void, Diagnostic> {
        if (!contract || !table) {
            return std::unexpected(failure(record, "Patch API publication request has a null contract or table"));
        }
        return record.api->publish(contract_view(*contract), table);
    });
}

// Copies a compatible declared provider table into out; the caller supplies contract.table_size writable bytes.
std::uint32_t FC_CDECL get_api(void *owner, FC_Text provider, const FC_ApiContract *contract, void *out, FC_ErrorSink error) {
    return api_operation(owner, error, [&](PatchRecord &record) -> std::expected<void, Diagnostic> {
        if (!contract || !out || (provider.size && !provider.data)) {
            return std::unexpected(failure(record, "Required patch API request has a null contract, output or provider data pointer"));
        }

        auto result = record.api->get(fc::detail::view(provider), contract_view(*contract));
        if (!result) {
            return std::unexpected(std::move(result.error()));
        }

        // Return a value copy while the function contexts continue to refer to installed provider objects.
        std::memcpy(out, result->data(), result->size());
        return {};
    });
}

// Reserves an optional notification against the selected provider; the consumer retains callback and context.
std::uint32_t FC_CDECL on_available(void *owner, FC_Text provider, const FC_ApiContract *contract, void *context, FC_ApiAvailable callback,
                                    FC_ErrorSink error) {
    return api_operation(owner, error, [&](PatchRecord &record) -> std::expected<void, Diagnostic> {
        if (!contract || (provider.size && !provider.data)) {
            return std::unexpected(failure(record, "Optional patch API notification request has a null contract or provider data pointer"));
        }
        return record.api->on_available(fc::detail::view(provider), contract_view(*contract), context, callback,
                                        record.runtime->selection_session());
    });
}

// Runs a callback synchronously with consumer attribution, or returns 0 before calling an inactive consumer's binding.
std::uint32_t FC_CDECL invoke(void *identity, FC_ConsumerInvoke callback, void *context) {
    const auto *owner = static_cast<PatchRecord *>(identity);
    // Acquire activation's prepared state before the SDK thunk touches the consumer's bound object.
    if (!owner || !callback || owner->state.load(std::memory_order_acquire) != PatchState::Installed) {
        return 0;
    }

    PatchCallbackScope scope{static_cast<PatchRecord *>(identity)};
    fc::invoke_patch_callback(callback, context);
    return 1;
}

} // namespace

const FC_PatchApi *patch_api_host() noexcept {
    static const FC_PatchApi table{sizeof(FC_PatchApi), &publish_api, &get_api, &on_available, &invoke};
    return &table;
}

} // namespace fc::runtime
