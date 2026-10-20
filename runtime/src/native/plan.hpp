#pragma once

#include "images/image.hpp"
#include "bridge.hpp"
#include "native_allocation.hpp"
#include "native_call.hpp"
#include "native_memory.hpp"

#include <fc/base.hpp>

#include <memory>
#include <optional>
#include <span>
#include <variant>

namespace fc::runtime {

struct PatchRecord;
struct LiveImageObservation;
class HookRegistry;
class HookPreparation;
class SubmittedPlan;

// Describes a selected image for live patching or offline validation.
struct PlanImage {
    // Selection label and recognized build ID exposed through the plan boundary.
    std::string name, profile;
    // Actual live base or preferred offline base; size is SizeOfImage in bytes.
    std::uintptr_t base{};
    std::uint32_t size{};
    // Offline permission checks use these section ranges at the preferred base.
    std::vector<LoadedSection> sections;
    // Empty for live images; otherwise the caller keeps this mapped storage alive.
    std::span<const std::byte> mapped;
};

// Copies image bindings from a ready selection; observed live modules must already be pinned.
std::expected<std::vector<PlanImage>, Diagnostic> selected_plan_images(const PatchRecord &, std::span<const LiveImageObservation>);

// Maps validated file sections into zero-filled storage; the caller retains it while any plan borrows it.
std::expected<std::vector<std::byte>, Diagnostic> map_file_image(const FileImage &, std::span<const std::byte> file);

// Copies the parsed image's metadata and borrows matching storage returned by map_file_image.
// The caller keeps the mapped storage alive while any plan uses it.
PlanImage mapped_plan_image(std::string name, const FileImage &, std::span<const std::byte> mapped);

// Identifies a byte within one selected image and retains its author-supplied diagnostic label.
struct LocationRecord {
    // Index into the owning plan's selected images.
    std::size_t image{};
    // Relative virtual address (RVA): the byte offset from the image base.
    std::uint32_t rva{};
    std::string name;
};

// Owns the expected bytes or target used to verify an operation's original image contents.
struct EvidenceRecord {
    std::uint32_t kind{};
    std::vector<std::byte> bytes, mask;
    std::optional<LocationRecord> target;
};

enum class ResourceKind {
    Data,
    Memory,
    Call,
    Fault
};

// Retains prepared memory, a native call or a fault site for an opaque SDK handle.
// The resource record stays at the same address as the plan grows.
struct NativeResource {
    ResourceKind kind{};
    // The owning plan stays alive while a plugin can use this resource handle.
    SubmittedPlan *plan{};
    // Prepared memory or call address; size is bytes and count is allocated elements.
    std::uintptr_t address{};
    std::uint64_t size{}, count{};
    // Expected-fault resources retain the declared read, write or execute access kind.
    std::uint32_t fault_kind{};
    // Ready permits resource callbacks; writable controls continuing memory access.
    bool writable{}, ready{};
    // Continuing access to replacement data borrows the allocation resource from the same plan.
    NativeResource *allocation{};
    // Owns the allocation for replacement data.
    patching::NativeAllocation storage;
    // Owns a generated frame invoker for a custom call.
    native::ExecutableBridge invoker;
    // Shared hooks may supply an invoker owned by the hook registry instead of this resource.
    const native::ExecutableBridge *borrowed_invoker{};
};

// Retains an image location, allocation offset or plugin function until its address can be resolved.
struct AddressRecord {
    std::uint32_t kind{};
    std::optional<LocationRecord> location;
    EvidenceRecord evidence;
    NativeResource *allocation{};
    std::uint64_t offset{};
    std::uintptr_t function{};
};

// Reserves snapshot, continuing memory or expected-fault access to an image or allocation range.
struct ReadClaim {
    LocationRecord location;
    EvidenceRecord evidence;
    std::uint64_t size{};
    std::uint32_t mode{};
    bool writable{};
    NativeResource *resource{};
};

// Owns replacement bytes and, for instruction edits, the original bytes checked before installation.
struct ByteWrite {
    LocationRecord location;
    EvidenceRecord evidence;
    std::vector<std::byte> bytes, original;
    bool instructions{};
};

// Retains original instructions and a checked encoding, deferring allocation addresses until preparation.
struct OperandEdit {
    LocationRecord location;
    EvidenceRecord evidence;
    bool address_value{};
    AddressRecord address;
    // Known encodings are retained as their byte pattern; allocation-dependent values remain empty.
    std::optional<std::uint64_t> encoded;
    std::vector<std::byte> original;
    // Byte offset and byte width within original; relative fields use the instruction end as their base.
    std::uint32_t field_offset{}, field_size{};
    bool encoded_signed{}, relative{};
};

// Requests aligned replacement data, optionally constrained near a selected image.
struct Allocation {
    NativeResource *resource{};
    std::uint32_t alignment{};
    std::optional<std::size_t> near_image;
    std::vector<std::byte> initial;
};

// Writes an absolute pointer or a four-byte displacement measured from the end of its field.
struct AddressFixup {
    LocationRecord location;
    EvidenceRecord evidence;
    AddressRecord target;
    bool relative{};
    std::optional<std::uint64_t> encoded;
};

// Redirects or inserts a direct branch and can retain a callable handle to the original target.
struct BranchEdit {
    LocationRecord location;
    EvidenceRecord evidence;
    AddressRecord target;
    std::uint32_t kind{};
    std::vector<std::byte> original;
    // Absolute target decoded from a redirected branch, retained for its optional original-call handle.
    std::uintptr_t original_target{};
    NativeResource *resource{};
    std::optional<CallDescription> call;
};

// Retains one callback declaration for a shared physical hook site.
struct HookParticipant {
    LocationRecord location;
    EvidenceRecord evidence;
    FC_HookParticipant value{};
    std::optional<CallDescription> call;
    std::vector<std::byte> original;
    NativeResource *resource{};
    // Registry-owned generated bridges remain alive after the participant is installed.
    const native::ExecutableBridge *entry{}, *invoker{};
};

// Reserves an executable entry and retains the call description used by its resource handle.
struct NativeCallOperation {
    LocationRecord location;
    EvidenceRecord evidence;
    CallDescription call;
    NativeResource *resource{};
};

using Operation =
    std::variant<ReadClaim, ByteWrite, OperandEdit, Allocation, AddressFixup, BranchEdit, HookParticipant, NativeCallOperation>;

enum class ClaimKind {
    Read,
    ContinuingRead,
    ContinuingWrite,
    Write,
    Callable,
    Hook
};

// Reserves a half-open address range for conflict checks within and between submitted plans.
struct MemoryClaim {
    SubmittedPlan *plan{};
    std::uintptr_t begin{}, end{};
    ClaimKind kind{};
    // Identifies the declaration so compatible hook and callable claims can share a site.
    std::size_t operation{};
    // Null denotes live addresses; offline inputs may share preferred bases while owning distinct mappings.
    const std::byte *offline_image{};
};

// Owns the final bytes to copy into a live image after revalidation succeeds.
struct PreparedWrite {
    std::uintptr_t address{};
    std::vector<std::byte> bytes;
    LocationRecord location;
};

// Collects a patch's native requests, validates them and retains the resources needed to install them.
class SubmittedPlan {
  public:
    // Owns selected image records; supplied owner and hook registry remain alive while the plan uses them.
    SubmittedPlan(PatchRecord *owner, std::vector<PlanImage> images, HookRegistry *hooks = nullptr);
    ~SubmittedPlan();
    SubmittedPlan(const SubmittedPlan &) = delete;
    SubmittedPlan &operator=(const SubmittedPlan &) = delete;

    // Returns callbacks and image labels that borrow this plan for the planning phase.
    FC_Plan boundary() noexcept;
    // Retains the first failure so later requests report the same cause.
    void fail(Error error);

    const std::optional<Error> &error() const noexcept {
        return error_;
    }

    // Supplies the qualified patch ID for conflict diagnostics and shared-hook ordering.
    std::string owner_id() const;
    // Copies a planning request; a produced handle becomes usable after successful preparation.
    std::expected<void, Error> submit(const FC_Operation &, void **resource);
    // Copies a snapshot to caller storage and reserves its bytes for conflict checks during Plan.
    // The output buffer must hold the claim's requested byte count.
    std::expected<void, Error> read(const FC_ReadClaim &, void *out);
    // Resolves a case-insensitive profile ID and bounds its RVA within the selected image.
    std::expected<LocationRecord, Error> location(FC_Location) const;
    // Returns owned bytes, substituting saved hook originals when the installed site still matches.
    std::expected<std::vector<std::byte>, Error> bytes(const LocationRecord &, std::size_t, bool executable = false) const;
    // Checks image bounds and live page or offline section permissions for a nonempty byte range.
    std::expected<void, Error> access(const LocationRecord &, std::size_t, bool executable = false) const;
    // Verifies expected bytes or targets, reusing supplied observations and reading any missing tail.
    std::expected<void, Error> evidence(const LocationRecord &, const EvidenceRecord &, std::span<const std::byte> observed = {}) const;
    // Resolves a retained address, requiring prepared allocations or current image and function mappings.
    std::expected<std::uintptr_t, Error> address(const AddressRecord &, bool executable = false) const;
    // Reports the first retained failure or incompatible overlap within this plan.
    std::expected<void, Error> validate() const;

    // Preparation completes storage and call targets before the author Prepare callback.
    std::expected<void, Error> prepare();
    // Rechecks evidence, instruction originals and permissions immediately before installation.
    std::expected<void, Error> revalidate() const;
    // Applies prepared writes and hooks; unsafe failure means native changes may already be exposed.
    std::expected<void, patching::NativeWriteFailure> install();

    const std::vector<PlanImage> &images() const noexcept {
        return images_;
    }

    const std::vector<Operation> &operations() const noexcept {
        return operations_;
    }

    const std::vector<MemoryClaim> &claims() const noexcept {
        return claims_;
    }

    const std::vector<PreparedWrite> &writes() const noexcept {
        return writes_;
    }

    PatchRecord *owner() const noexcept {
        return owner_;
    }

    // Exposure prevents the owner from releasing storage that native code may now use.
    bool exposed() const noexcept {
        return exposed_;
    }

    // Closes initialization views and prevents disposal of storage that native code may use.
    void mark_exposed() noexcept {
        exposed_ = true;
    }

  private:
    friend class HookRegistry;

    // Copies evidence bytes and resolves selected-image targets, rejecting malformed evidence descriptions.
    std::expected<EvidenceRecord, Error> copy_evidence(FC_Evidence) const;
    // Copies an address description and checks its image evidence, allocation ownership or plugin function mapping.
    std::expected<AddressRecord, Error> copy_address(FC_Address) const;
    // Creates a plan-owned resource whose pointer remains a stable opaque handle as more requests are submitted.
    NativeResource *resource(ResourceKind);

    PatchRecord *owner_{};
    HookRegistry *hooks_{};
    // Factory's module against which supplied callback and function-target addresses are checked.
    void *plugin_module_{};
    std::vector<PlanImage> images_;
    // ABI views point into images_, whose strings stay unchanged after construction.
    std::vector<FC_PlanImage> bindings_;
    std::vector<Operation> operations_;
    std::vector<std::unique_ptr<NativeResource>> resources_;
    std::vector<MemoryClaim> claims_;
    std::vector<PreparedWrite> writes_;
    // Executable relays keep out-of-range branch targets reachable after installation.
    std::vector<patching::NativeAllocation> relays_;
    std::unique_ptr<HookPreparation> hook_preparation_;
    std::optional<Error> error_;
    // Preparation closes submission; exposure prevents disposal after any live mutation takes effect.
    bool preparing_{}, exposed_{};
};

// Checks ready plans against installed claims in the caller's dependency order.
// Rejected claims remain rejected if an earlier patch fails later.
std::vector<std::pair<SubmittedPlan *, Error>> validate_plans(std::span<SubmittedPlan *const> ready,
                                                              std::span<const MemoryClaim> installed);
// Returns the process-lifetime ABI table for prepared native resource handles.
const FC_NativeApi *native_api() noexcept;

} // namespace fc::runtime
