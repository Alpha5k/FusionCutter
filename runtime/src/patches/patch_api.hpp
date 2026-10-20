#pragma once

#include "diagnostic.hpp"

#include <fc/abi.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fc::runtime {

struct PatchRecord;
struct SelectionSession;

// Describes the table layout and function signatures that providers and consumers must share.
struct PatchApiContract {
    // Case-sensitive, versioned API ID; the caller keeps this and the spans alive during reservation calls.
    std::string_view id;
    // Complete table extent in bytes.
    std::uint32_t table_size{};
    // Power-of-two alignment in bytes; table_size must be a multiple of this value.
    std::uint32_t table_alignment{};
    // Opaque comparison bytes include member signatures and declared record layouts.
    std::span<const std::byte> description;
    // Byte offsets of required function pointers within the table, excluding their bound contexts.
    std::span<const std::uint32_t> required_functions;
};

// Receives the provider's aligned table synchronously on the consumer's control thread.
using PatchApiAvailable = FC_ApiAvailable;

// Owns one patch's copied API tables and optional subscriptions; the control thread performs every operation.
// The selected patch record outlives it; installed tables and callback contexts remain valid until process exit.
class PatchApiReservations {
  public:
    explicit PatchApiReservations(PatchRecord &owner) noexcept;
    PatchApiReservations(const PatchApiReservations &) = delete;
    PatchApiReservations &operator=(const PatchApiReservations &) = delete;

    // Copies contract.table_size bytes from table during Prepare; table must contain that many readable bytes.
    // Rejects invalid extents or a repeated case-sensitive API ID.
    std::expected<void, Diagnostic> publish(const PatchApiContract &, const void *table);

    // Requires Prepare, a resolved provider dependency and an Installed provider with a compatible, callable table.
    // The returned aligned bytes borrow the provider's process-lifetime reservation.
    std::expected<std::span<const std::byte>, Diagnostic> get(std::string_view provider, const PatchApiContract &) const;

    // Copies an API notification request during Prepare; a declared dependency is unnecessary.
    // Callback and context must survive until delivery or discarded preparation; a provider absent from the session never delivers.
    std::expected<void, Diagnostic> on_available(std::string_view provider, const PatchApiContract &, void *context, PatchApiAvailable,
                                                 const SelectionSession &);

    // Call on the consumer's control callback after Installed publication; both owners must be Installed.
    // Delivers each subscription once or appends one connection diagnostic, including for late providers.
    void deliver_available(std::vector<Diagnostic> &diagnostics);

    // Releases tables and subscriptions after startup fails before native changes take effect.
    void discard() noexcept;

    // The host records its first failed Prepare operation so ignoring a Result cannot publish partial setup.
    void fail(Diagnostic diagnostic);

    const std::optional<Diagnostic> &error() const noexcept {
        return error_;
    }

  private:
    // Owned contract data uses the same byte extents and pointer offsets as PatchApiContract.
    struct Contract {
        std::string id;
        std::uint32_t table_size{}, table_alignment{};
        std::vector<std::byte> description;
        std::vector<std::uint32_t> required_functions;
    };

    struct AlignedDelete {
        std::size_t alignment{};

        // Frees table storage with the alignment used for its allocation.
        void operator()(std::byte *) const noexcept;
    };

    struct Publication {
        Contract contract;
        // Separate aligned allocation keeps the table address stable when reservations grow.
        std::unique_ptr<std::byte, AlignedDelete> table;
    };

    struct Subscription {
        // Pending provider identity; cleared before its notification is delivered or rejected.
        PatchRecord *provider{};
        Contract contract;
        // Consumer-owned callable state must survive until delivery or discarded preparation.
        void *context{};
        PatchApiAvailable callback{};
    };

    // Copies all borrowed contract data into owned storage for use after Prepare returns.
    static Contract copy_contract(const PatchApiContract &);
    // Returns borrowed provider bytes after exact contract comparison and required pointer checks.
    std::expected<std::span<const std::byte>, Diagnostic> connect(const PatchApiReservations &provider, const PatchApiContract &) const;

    // Stable identity supplied by selection/dependency resolution.
    PatchRecord *owner_{};
    std::vector<Publication> publications_;
    std::vector<Subscription> subscriptions_;
    std::optional<Diagnostic> error_;
};

// Returns the outermost patch callback's owner on this thread, or null outside patch callbacks.
PatchRecord *current_consumer() noexcept;

// Establishes attribution when entering the outermost callback and returns the previous owner for restoration.
[[nodiscard]] PatchRecord *enter_patch_callback(PatchRecord *) noexcept;

// Restores the owner returned by enter_patch_callback when leaving that callback.
void leave_patch_callback(PatchRecord *) noexcept;

// Restores thread-local patch attribution when a callback returns or unwinds through an exception.
class PatchCallbackScope {
  public:
    // Enters this callback's attribution scope; owner must remain valid throughout the callback.
    explicit PatchCallbackScope(PatchRecord *owner) noexcept : previous_(enter_patch_callback(owner)) {}

    ~PatchCallbackScope() {
        leave_patch_callback(previous_);
    }

    PatchCallbackScope(const PatchCallbackScope &) = delete;
    PatchCallbackScope &operator=(const PatchCallbackScope &) = delete;

  private:
    PatchRecord *previous_;
};

// Returns the process-lifetime ABI table for owner-bound Prepare operations and installed consumer callbacks.
const FC_PatchApi *patch_api_host() noexcept;

} // namespace fc::runtime
