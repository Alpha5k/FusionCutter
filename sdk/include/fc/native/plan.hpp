#pragma once

#include <fc/contexts.hpp>
#include <fc/native/handles.hpp>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace fc {

// Submits native requests during Plan; copies share the first failure, and RVA/typed sites use the bound image.
// Callbacks run on the native thread reaching the site; keep objects referenced by callbacks alive and synchronize shared access.
class Plan {
  public:
    // Borrows the runtime plan and handler-owned capture storage for the duration of the planning callback.
    Plan(FC_Plan *plan, detail::StageAccess access, detail::CallbackCaptures *captures = nullptr)
        : plan_(plan), access_(access), captures_(captures) {
        // One selected image binds bare RVAs automatically; multiple images require for_image before using them.
        if (plan && plan->image_count == 1) {
            image_ = detail::view(plan->images[0].profile);
        }
    }

    Logger log() const {
        return Logger{access_};
    }

    // Return the bound selected profile ID; missing binding records plan failure and returns an empty view.
    std::string_view profile() const {
        if (image_.empty()) {
            fail("Bind a selected image before requesting its profile");
        }
        return image_;
    }

    // Bind a copied view by ASCII case-insensitive requirement label; an unknown label records plan failure.
    Plan for_image(std::string_view name) const {
        Plan result = *this;
        result.image_.clear();
        // Requirement names resolve to selected build profiles, which are used by every location from this view.
        if (plan_) {
            for (std::uint32_t i = 0; i < plan_->image_count; ++i) {
                if (detail::equal_id(name, detail::view(plan_->images[i].name))) {
                    result.image_ = detail::view(plan_->images[i].profile);
                    return result;
                }
            }
        }

        result.fail("Plan image label is not selected");
        return result;
    }

    // Describe an RVA in the bound image with an optional diagnostic label; missing binding records failure.
    Location at(std::uint32_t rva, std::string_view name = {}) const {
        Location result{image_, rva, std::string{name}, image_.empty()};
        if (result.invalid) {
            fail("Bind a selected image before using a bare native RVA");
        }
        return result;
    }

    // Record a planning failure; the runtime keeps the first error and rejects later submissions.
    void fail(std::string_view message) const {
        FC_Operation operation{};
        operation.kind = FC_OPERATION_READ;
        operation.value.read.mode = FC_READ_FAILURE;
        operation.value.read.failure = detail::text(message);
        submit(operation);
    }

    // Copy a trivially copyable value during Plan and reserve its range/evidence for later revalidation.
    template <class T> Result<T> read(Location location, Evidence evidence = {}) const;

    template <class T> Result<T> read(std::uint32_t rva, Evidence evidence = {}) const;

    // Read the typed value with its evidence; return Result<T> for Count == 1, otherwise Result<std::array<T, Count>>.
    template <class T, std::size_t Count> auto read(const DataLocation<T, Count> &site) const;

    // Copy a trivially copyable value into the plan for installation; require alignof(T) at the destination.
    template <class T> void write(Location location, const T &value, Evidence evidence = {}) const;

    template <class T> void write(std::uint32_t rva, const T &value, Evidence evidence = {}) const;

    // Queue the typed value or array with the site's evidence and invalid flag; submission copies its bytes.
    template <class T, std::size_t Count>
    void write(const DataLocation<T, Count> &site, const std::conditional_t<Count == 1, T, std::array<T, Count>> &value) const;

    // Copy arbitrary replacement bytes into the plan with byte alignment and optional preimage evidence.
    void write(Location location, Bytes bytes, Evidence evidence = {}) const;

    // Reserve count zero-initialized trivially copyable elements; Data::values becomes mutable during Prepare.
    template <class T> Data<T> allocate(std::size_t count, Placement options = {}) const;

    // Reserve fixed storage initialized from a copied span; the runtime owns it for the patch's lifetime.
    template <class T> Data<T> allocate(std::span<const T> values, Placement options = {}) const;

    // Reserve fixed runtime storage initialized from a copied array; elements must be trivially copyable.
    template <class T, std::size_t N> Data<T> allocate(const std::array<T, N> &values, Placement options = {}) const;

    template <class T, std::size_t N> Data<T> allocate(const T (&values)[N], Placement options = {}) const;

    // Reserve continuing access to count elements; const T requests read-only access, otherwise read/write access.
    template <class T> Memory<T> access(Location location, std::size_t count = 1, Evidence evidence = {}) const;

    // Reserve continuing access to this plan's allocation, usable after preparation and after publication.
    template <class T> Memory<T> access(const Data<std::remove_const_t<T>> &data) const;

    // Reserve the bound-image range for prepared access; const T requests read-only permission.
    template <class T, std::size_t Count> Memory<T> access(const DataLocation<T, Count> &site) const;

    template <class T> Memory<T> access(std::uint32_t rva, std::size_t count = 1, Evidence evidence = {}) const;

    // Snapshot a nonempty count-element range of trivially copyable T during Plan.
    // Invalid extents or runtime read failures return Error.
    template <class T> Result<std::vector<T>> read_array(Location location, std::size_t count, Evidence evidence = {}) const;

    template <class T> Result<std::vector<T>> read_array(std::uint32_t rva, std::size_t count, Evidence evidence = {}) const;

    // Snapshot a nonempty byte range during Plan, retaining its read claim and optional evidence.
    Result<std::vector<std::byte>> read_bytes(Location location, std::size_t count, Evidence evidence = {}) const;

    // Checks a nonempty byte range and its evidence now and before installation; failure rejects the plan.
    // The checked range remains reserved against conflicting writes.
    void check(Location location, std::size_t count, Evidence evidence) const;

    void check(std::uint32_t rva, std::size_t count, Evidence evidence) const;

    void check(const CodeLocation &site, std::size_t count) const;

    // Copy a span's trivially copyable elements into the plan, requiring alignof(T) at the destination.
    template <class T, std::size_t Extent> void write(Location location, std::span<T, Extent> values, Evidence evidence = {}) const;

    void write_pointer(std::uint32_t rva, Address target, Evidence evidence = {}) const;

    // Queue 0x90 NOPs over exactly size complete instruction bytes, retaining any typed-site evidence.
    void nop(std::uint32_t rva, std::size_t size, Evidence evidence = {}) const;

    void nop(const CodeLocation &site, std::size_t size) const;

    // Install a pointer-sized absolute target address; allocation addresses resolve during preparation.
    void write_pointer(Location location, Address target, Evidence evidence = {}) const;

    // Install a four-byte signed displacement from the field's end; known out-of-reach targets reject Plan.
    void write(Location location, RelativeAddress target, Evidence evidence = {}) const;

    // Replace exactly size complete instruction bytes with 0x90 NOPs; size is measured in bytes.
    void nop(Location location, std::size_t size, Evidence evidence = {}) const;

    // Replace a zero-based decoded explicit operand's immediate/displacement; preserve its encoded width and signedness.
    template <class T> void operand(Location location, std::uint32_t index, Immediate<T> value, Evidence evidence = {}) const;

    template <class T> void operand(const CodeLocation &site, std::uint32_t index, Immediate<T> value) const;

    // Replace a decoded operand field with an address, deriving relative displacement when its encoding requires it.
    void operand(Location location, std::uint32_t index, Address target, Evidence evidence = {}) const;

    void operand(const CodeLocation &site, std::uint32_t index, Address target) const;

    // Reserve a callable entry; Call is a compiler function pointer or explicit NativeCall, usable after Prepare.
    template <class Call> NativeFunction<Call> function(Location location, Evidence evidence = {}) const;

    template <class Call> NativeFunction<Call> function(std::uint32_t rva, Evidence evidence = {}) const;

    template <class Call> NativeFunction<Call> function(const FunctionLocation<Call> &site) const;

    // Replace a function entry with a noexcept typed callback; return its prepared original-call handle.
    // Callbacks may accept const NativeContext & before the native arguments; the handler retains the callable.
    template <class Call, class F> NativeFunction<Call> hook(Location location, F callback, Evidence evidence = {}) const;

    // Replace one direct CALL site's callee with a noexcept typed callback and retain a handle to that callee.
    template <class Call, class F> NativeFunction<Call> hook_call(Location location, F callback, Evidence evidence = {}) const;

    // Replace a bound-image function with a noexcept typed callback and retain its prepared original-call handle.
    template <class Call, class F> NativeFunction<Call> hook(const FunctionLocation<Call> &site, F callback) const;

    // Install a noexcept callback taking CpuContext &; register edits determine the resumed native machine state.
    template <class F> void hook_instruction(Location location, F callback, Evidence evidence = {}) const;

    // Observe const native arguments; after also receives the result. Optional NativeContext precedes the arguments.
    // Optional State is trivially copyable and noexcept default-constructed: mutable before the call, const after.
    template <class Call, class State = void, class F> void observe(Location location, Before<F> callback, Evidence evidence = {}) const;

    template <class Call, class State = void, class F> void observe(Location location, After<F> callback, Evidence evidence = {}) const;

    template <class Call, class State = void, class B, class A>
    void observe(Location location, Before<B> first, After<A> last, Evidence evidence = {}) const;

    template <class State = void, class Call, class B, class A>
    void observe(const FunctionLocation<Call> &site, Before<B> first, After<A> last) const;

    // Observe one direct CALL's callee with the same const argument/result and per-invocation State conventions.
    template <class Call, class State = void, class F>
    void observe_call(Location location, Before<F> callback, Evidence evidence = {}) const;

    template <class Call, class State = void, class F>
    void observe_call(Location location, After<F> callback, Evidence evidence = {}) const;

    template <class Call, class State = void, class B, class A>
    void observe_call(Location location, Before<B> first, After<A> last, Evidence evidence = {}) const;

    template <class State = void, class Call, class B, class A>
    void observe_call(const CallLocation<Call> &site, Before<B> first, After<A> last) const;

    // Replace a bound-image direct CALL with a noexcept typed callback and retain its original callee handle.
    template <class Call, class F> NativeFunction<Call> hook_call(const CallLocation<Call> &site, F callback) const;

    template <class State = void, class Call, class F> void observe(const FunctionLocation<Call> &site, Before<F> callback) const;

    template <class State = void, class Call, class F> void observe(const FunctionLocation<Call> &site, After<F> callback) const;

    template <class State = void, class Call, class F> void observe_call(const CallLocation<Call> &site, Before<F> callback) const;

    template <class State = void, class Call, class F> void observe_call(const CallLocation<Call> &site, After<F> callback) const;

    // Observe const CpuContext with noexcept callbacks around modifiers, before resuming native instructions.
    // Optional State is trivially copyable and noexcept default-constructible; before receives it mutable, after const.
    template <class State = void, class B, class A>
    void observe_instruction(const CodeLocation &site, Before<B> first, After<A> last) const;

    template <class F> void observe_instruction(const CodeLocation &site, Before<F> callback) const;

    template <class F> void observe_instruction(const CodeLocation &site, After<F> callback) const;

    // Install a noexcept CpuContext & modifier at the typed site or bound-image RVA, retaining evidence.
    template <class F> void hook_instruction(const CodeLocation &site, F callback) const;

    // Reserve recovery at this exact instruction for one access-violation kind; invoke it through the prepared handle.
    ExpectedFault expect_fault(Location location, NativeException kind, Evidence evidence = {}) const;

    // Reserve the bound-image instruction and access-violation kind for synchronous prepared-handle recovery.
    ExpectedFault expect_fault(const CodeLocation &site, NativeException kind) const;

    // Observe the hook's saved CpuContext as const; instruction observers run around modifiers before native resumption.
    template <class F> void observe_instruction(Location location, Before<F> callback, Evidence evidence = {}) const;

    template <class F> void observe_instruction(Location location, After<F> callback, Evidence evidence = {}) const;

    template <class State = void, class B, class A>
    void observe_instruction(Location location, Before<B> first, After<A> last, Evidence evidence = {}) const;

    template <class Call, class State = void, class F> void observe(std::uint32_t rva, Before<F> callback, Evidence evidence = {}) const;

    template <class Call, class State = void, class F> void observe(std::uint32_t rva, After<F> callback, Evidence evidence = {}) const;

    template <class Call, class State = void, class B, class A>
    void observe(std::uint32_t rva, Before<B> first, After<A> last, Evidence evidence = {}) const;

    template <class Call, class State = void, class F>
    void observe_call(std::uint32_t rva, Before<F> callback, Evidence evidence = {}) const;

    template <class Call, class State = void, class F>
    void observe_call(std::uint32_t rva, After<F> callback, Evidence evidence = {}) const;

    template <class Call, class State = void, class B, class A>
    void observe_call(std::uint32_t rva, Before<B> first, After<A> last, Evidence evidence = {}) const;

    template <class F> void observe_instruction(std::uint32_t rva, Before<F> callback, Evidence evidence = {}) const;

    template <class F> void observe_instruction(std::uint32_t rva, After<F> callback, Evidence evidence = {}) const;

    template <class State = void, class B, class A>
    void observe_instruction(std::uint32_t rva, Before<B> first, After<A> last, Evidence evidence = {}) const;

    template <class Call, class F> NativeFunction<Call> hook(std::uint32_t rva, F callback, Evidence evidence = {}) const;

    template <class Call, class F> NativeFunction<Call> hook_call(std::uint32_t rva, F callback, Evidence evidence = {}) const;

    template <class F> void hook_instruction(std::uint32_t rva, F callback, Evidence evidence = {}) const;

    // Edit a zero-based explicit operand at a bound-image RVA; preserve encoded width and check value/address fit.
    template <class T> void operand(std::uint32_t rva, std::uint32_t index, Immediate<T> value, Evidence evidence = {}) const;

    void operand(std::uint32_t rva, std::uint32_t index, Address target, Evidence evidence = {}) const;

    void redirect_call(std::uint32_t rva, Address target, Evidence evidence = {}) const;

    void redirect_jump(std::uint32_t rva, Address target, Evidence evidence = {}) const;

    void insert_call(std::uint32_t rva, std::size_t size, Address target, Evidence evidence = {}) const;

    void insert_jump(std::uint32_t rva, std::size_t size, Address target, Evidence evidence = {}) const;

    ExpectedFault expect_fault(std::uint32_t rva, NativeException kind, Evidence evidence = {}) const;

    // Retarget an existing five-byte direct relative CALL, requiring executable target storage.
    void redirect_call(Location location, Address target, Evidence evidence = {}) const;

    // Retarget an existing five-byte direct unconditional JMP, requiring executable target storage.
    void redirect_jump(Location location, Address target, Evidence evidence = {}) const;

    // Insert a direct CALL across exactly size complete instruction bytes, at least five; fill trailing bytes with NOPs.
    void insert_call(Location location, std::size_t size, Address target, Evidence evidence = {}) const;

    // Insert a direct JMP across exactly size complete instruction bytes, at least five; fill trailing bytes with NOPs.
    void insert_jump(Location location, std::size_t size, Address target, Evidence evidence = {}) const;

    // Replaces complete instruction bytes while retaining the typed code site's evidence.
    void write(const CodeLocation &site, Bytes values) const;

    Result<std::vector<std::byte>> read_bytes(std::uint32_t rva, std::size_t count, Evidence evidence = {}) const;

    // Retarget a direct CALL to a typed image function and return a prepared handle to the original callee.
    template <class Call>
    NativeFunction<Call> redirect_call(Location source, const FunctionLocation<Call> &target, Evidence evidence = {}) const;

    // Retarget a direct JMP to a typed image function and return a prepared handle to its original destination.
    template <class Call>
    NativeFunction<Call> redirect_jump(Location source, const FunctionLocation<Call> &target, Evidence evidence = {}) const;

    // Retarget a direct CALL to plugin code with the same compiler signature; retain the original callee handle.
    template <class Call>
        requires(std::is_pointer_v<Call> && std::is_function_v<std::remove_pointer_t<Call>>)
    NativeFunction<Call> redirect_call(Location source, Call target, Evidence evidence = {}) const;

    // Retarget a direct JMP to plugin code with the same compiler signature; retain the original destination handle.
    template <class Call>
        requires(std::is_pointer_v<Call> && std::is_function_v<std::remove_pointer_t<Call>>)
    NativeFunction<Call> redirect_jump(Location source, Call target, Evidence evidence = {}) const;

    // Redirect a five-byte direct CALL with matching signatures; retain a prepared handle to its previous callee.
    template <class Call> NativeFunction<Call> redirect_call(const CodeLocation &source, const FunctionLocation<Call> &target) const;

    template <class Call>
        requires(std::is_pointer_v<Call> && std::is_function_v<std::remove_pointer_t<Call>>)
    NativeFunction<Call> redirect_call(const CodeLocation &source, Call target) const;

    template <class Call>
    NativeFunction<Call> redirect_call(std::uint32_t rva, const FunctionLocation<Call> &target, Evidence evidence = {}) const;

    template <class Call>
        requires(std::is_pointer_v<Call> && std::is_function_v<std::remove_pointer_t<Call>>)
    NativeFunction<Call> redirect_call(std::uint32_t rva, Call target, Evidence evidence = {}) const;

    // Redirect a five-byte direct JMP with matching signatures; retain a prepared handle to its previous destination.
    template <class Call> NativeFunction<Call> redirect_jump(const CodeLocation &source, const FunctionLocation<Call> &target) const;

    template <class Call>
        requires(std::is_pointer_v<Call> && std::is_function_v<std::remove_pointer_t<Call>>)
    NativeFunction<Call> redirect_jump(const CodeLocation &source, Call target) const;

    template <class Call>
    NativeFunction<Call> redirect_jump(std::uint32_t rva, const FunctionLocation<Call> &target, Evidence evidence = {}) const;

    template <class Call>
        requires(std::is_pointer_v<Call> && std::is_function_v<std::remove_pointer_t<Call>>)
    NativeFunction<Call> redirect_jump(std::uint32_t rva, Call target, Evidence evidence = {}) const;

    template <class Call, class Target> NativeFunction<Call> redirect_call(const CallLocation<Call> &site, Target target) const;

  private:
    // Describe a typed image target with its evidence, or plugin code whose compiler signature matches Call.
    template <class Call> Address target_address(const FunctionLocation<Call> &target) const;

    template <class Call, class Target>
        requires(std::is_pointer_v<Target> && std::is_function_v<std::remove_pointer_t<Target>>)
    Address target_address(Target target) const;

    // Queue a typed five-byte CALL or JMP redirection and retain its original destination for prepared calls.
    template <class Call, class Target>
    NativeFunction<Call> typed_branch(Location source, Target target, std::uint32_t kind, Evidence evidence) const;

    // Return native services if the host table includes them; older tables may end before this service pointer.
    const FC_NativeApi *api() const {
        return detail::table_available(access_.host, offsetof(FC_Host, native), sizeof(access_.host->native)) ? access_.host->native
                                                                                                              : nullptr;
    }

    // Submit synchronously so the runtime can copy borrowed descriptions and record failures; return any requested resource.
    void *submit(const FC_Operation &operation) const {
        void *resource{};
        if (plan_ && plan_->submit) {
            plan_->submit(plan_->context, &operation, &resource, {});
        }
        return resource;
    }

    // Reserve count elements with copied initializer bytes or zero fill, using the requested placement and alignment.
    template <class T> Data<T> allocate_bytes(std::size_t count, std::span<const std::byte> bytes, const Placement &options) const;

    // Queue an absolute pointer or signed four-byte relative address field with its source and target evidence.
    void fixup(const Location &location, Address target, bool relative, Evidence evidence) const;

    // Queue a direct branch over size bytes; optional call metadata requests an original-destination resource.
    void *branch(Location location, Address target, std::uint32_t kind, bool insert, std::size_t size, const FC_NativeCall *call,
                 Evidence evidence) const;

    // Retain a typed replacement callable for the selected hook scope and return its prepared original-call handle.
    template <class Call, class F>
    NativeFunction<Call> modifier(Location location, F callback, Evidence evidence, std::uint32_t scope) const;

    // Retain before/after callables for the selected hook scope; optional State is initialized per invocation.
    template <class Call, class State, class B, class A>
    void observer(Location location, B first, A last, Evidence evidence, std::uint32_t scope) const;

    FC_Plan *plan_{};
    detail::StageAccess access_;
    // Owned by the handler holder, which outlives installed callback invocations and the handler destructor.
    detail::CallbackCaptures *captures_{};
    // Selected profile ID for bare RVAs and typed sites, cleared in an unbound multi-image view.
    std::string image_;
};

} // namespace fc
