#pragma once

#include <fc/native/values.hpp>
#include <fc/detail/native_types.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <tuple>
#include <type_traits>
#include <utility>

namespace fc {

// Borrows fixed replacement-data storage owned by the runtime plan; retain the owning patch while using this handle.
template <class T> class Data {
  public:
    Data() = default;

    // Wrap an opaque allocation resource and its declared element count; normally returned by Plan::allocate.
    Data(void *resource, const FC_NativeApi *api, std::size_t size) : resource_(resource), api_(api), size_(size) {}

    // Returns the declared element count, even before preparation makes the storage available.
    std::size_t size() const {
        return size_;
    }

    // Describe an element plus a byte offset within T; index == size() permits the one-past-end address only.
    Address address(std::size_t index = 0, std::size_t member = 0) const {
        Address result;
        result.kind = FC_ADDRESS_ALLOCATION;
        result.resource = resource_;
        // Preserve invalid bounds in the returned description so a later plan submission rejects it.
        result.invalid = index > size_ || member >= sizeof(T) || (index == size_ && member) || index > UINT64_MAX / sizeof(T);
        if (!result.invalid) {
            result.offset = index * sizeof(T) + member;
        }
        return result;
    }

    // Borrow mutable initialization storage during Prepare; stop mutating it when the plan publishes native changes.
    // Unavailable storage returns an empty span.
    std::span<T> values() const {
        detail::NativeError error;
        void *values{};
        std::uint64_t count{};
        if (!detail::table_available(api_, offsetof(FC_NativeApi, data), sizeof(api_->data)) || !api_->data ||
            !api_->data(resource_, &values, &count, error.sink())) {
            return {};
        }
        return std::span{static_cast<T *>(values), static_cast<std::size_t>(count)};
    }

    void *resource() const {
        return resource_;
    }

  private:
    void *resource_{};
    const FC_NativeApi *api_{};
    std::size_t size_{};
};

// Borrows a runtime access resource; keep the owning patch and mapped object alive during every access.
// The author supplies synchronization, including while using a borrowed view.
template <class T> class Memory {
    static_assert(std::is_trivially_copyable_v<std::remove_const_t<T>>, "Native memory requires a trivially copyable element");

  public:
    Memory() = default;

    // Wrap a prepared-memory resource with its declared element count; normally returned by Plan::access.
    Memory(void *resource, const FC_NativeApi *api, std::size_t count) : resource_(resource), api_(api), count_(count) {}

    // Copy one zero-based element after checking its declared range and current native read access.
    Result<std::remove_const_t<T>> read(std::size_t index = 0) const {
        std::remove_const_t<T> value{};
        auto result = copy(&value, index, false);
        if (!result) {
            return std::unexpected(result.error());
        }
        return value;
    }

    // Copy one element into writable native memory; bounds and access failures return an Error.
    Result<> write(const T &value, std::size_t index = 0) const
        requires(!std::is_const_v<T>)
    {
        return copy(const_cast<T *>(&value), index, true);
    }

    // Copy the leading values.size() elements out; an empty span succeeds without accessing native memory.
    Result<> copy_to(std::span<std::remove_const_t<T>> values) const {
        return transfer(values.data(), values.size(), false);
    }

    // Copy values into the leading elements of writable native memory; an empty span succeeds.
    Result<> copy_from(std::span<const T> values) const
        requires(!std::is_const_v<T>)
    {
        return transfer(const_cast<T *>(values.data()), values.size(), true);
    }

    // Return a borrowed span after checking current access; the author retains lifetime and synchronization responsibility.
    Result<std::span<T>> view() const {
        detail::NativeError error;
        void *data{};
        std::uint64_t size{};
        if (!detail::table_available(api_, offsetof(FC_NativeApi, view), sizeof(api_->view)) || !api_->view ||
            !api_->view(resource_, &data, &size, error.sink())) {
            return std::unexpected(error.failure());
        }
        return std::span{static_cast<T *>(data), static_cast<std::size_t>(size / sizeof(T))};
    }

  private:
    // Copy a count-element prefix between caller storage and the native range using the declared access permission.
    Result<> transfer(void *value, std::size_t count, bool write) const {
        if (count > count_) {
            return std::unexpected(Error{"Memory copy exceeds its declared range"});
        }
        // Empty copies succeed even before the runtime resource is prepared.
        if (!count) {
            return {};
        }

        // The runtime ABI measures the transfer in bytes rather than elements.
        detail::NativeError error;
        if (!detail::table_available(api_, offsetof(FC_NativeApi, memory), sizeof(api_->memory)) || !api_->memory ||
            !api_->memory(resource_, 0, value, count * sizeof(T), write, error.sink())) {
            return std::unexpected(error.failure());
        }
        return {};
    }

    // Copy one zero-based element between caller storage and the native range; report bounds or access failures.
    Result<> copy(void *value, std::size_t index, bool write) const {
        if (index >= count_) {
            return std::unexpected(Error{"Memory element is outside its declared range"});
        }

        // Convert the checked element index to the runtime's byte offset.
        detail::NativeError error;
        if (!detail::table_available(api_, offsetof(FC_NativeApi, memory), sizeof(api_->memory)) || !api_->memory ||
            !api_->memory(resource_, index * sizeof(T), value, sizeof(T), write, error.sink())) {
            return std::unexpected(error.failure());
        }
        return {};
    }

    void *resource_{};
    const FC_NativeApi *api_{};
    std::size_t count_{};
};

// Borrows a prepared target or hook original; keep the owning patch alive and supply a valid native execution context.
// Result reports resource failures; faults inside the native target follow normal native exception handling.
template <class Call> class NativeFunction {
  public:
    NativeFunction() = default;

    // Wrap a retained call resource; normally supplied by Plan::function, hook or a typed redirection.
    NativeFunction(void *resource, const FC_NativeApi *api) : resource_(resource), api_(api) {}

    // Call synchronously after preparation; Call is a compiler function pointer or explicit NativeCall description.
    template <class... A> auto call(A &&...arguments) const -> Result<typename detail::FunctionTraits<Call>::Result> {
        using R = typename detail::FunctionTraits<Call>::Result;
        detail::NativeError error;
        // Compiler signatures need a resolved target; custom calls invoke their resource directly.
        if constexpr (std::is_pointer_v<Call>) {
            std::uintptr_t target{};
            if (!detail::table_available(api_, offsetof(FC_NativeApi, resolve), sizeof(api_->resolve)) || !api_->resolve ||
                !api_->resolve(resource_, &target, error.sink())) {
                return std::unexpected(error.failure());
            }
            if constexpr (std::is_void_v<R>) {
                reinterpret_cast<Call>(target)(std::forward<A>(arguments)...);
                return {};
            } else {
                return reinterpret_cast<Call>(target)(std::forward<A>(arguments)...);
            }
        } else {
            // Own converted logical arguments until the runtime bridge finishes using their frame pointers.
            using Arguments = typename detail::FunctionTraits<Call>::Arguments;
            Arguments values{std::forward<A>(arguments)...};
            std::array<const void *, sizeof...(A)> pointers{};
            std::apply(
                [&](auto &...value) {
                    std::size_t index{};
                    ((pointers[index++] = &value), ...);
                },
                values);
            // A void frame has no result storage; non-void calls fill a caller-owned result object.
            if constexpr (std::is_void_v<R>) {
                FC_NativeFrame frame{pointers.data(), nullptr, nullptr};
                if (!detail::table_available(api_, offsetof(FC_NativeApi, invoke), sizeof(api_->invoke)) || !api_->invoke ||
                    !api_->invoke(resource_, &frame, error.sink())) {
                    return std::unexpected(error.failure());
                }
                return {};
            } else {
                R result{};
                FC_NativeFrame frame{pointers.data(), &result, nullptr};
                if (!detail::table_available(api_, offsetof(FC_NativeApi, invoke), sizeof(api_->invoke)) || !api_->invoke ||
                    !api_->invoke(resource_, &frame, error.sink())) {
                    return std::unexpected(error.failure());
                }
                return result;
            }
        }
    }

  private:
    void *resource_{};
    const FC_NativeApi *api_{};
};

// Selects the exact access-violation kind declared by Plan::expect_fault.
enum class NativeException : std::uint32_t {
    ReadAccessViolation = FC_FAULT_READ,
    WriteAccessViolation = FC_FAULT_WRITE,
    ExecuteAccessViolation = FC_FAULT_EXECUTE
};

// Distinguishes an unusable handle, a completed callback and recovery from the declared native fault.
enum class FaultOutcome : std::uint32_t {
    Unavailable,
    Completed,
    Caught
};

// Borrows a planned fault resource; keep the owning patch alive during synchronous invocation.
// Only the exact planned instruction and access kind can be recovered.
class ExpectedFault {
  public:
    ExpectedFault() = default;

    // Wrap the planned fault resource; it becomes usable after the runtime prepares the owning plan.
    ExpectedFault(void *resource, const FC_NativeApi *api) : resource_(resource), api_(api) {}

    // Run a noexcept callback on this thread; recovery may skip its local destructors, so keep required cleanup outside.
    template <class F> FaultOutcome invoke(F &&callback) const noexcept {
        static_assert(std::is_nothrow_invocable_v<F &>, "ExpectedFault callbacks must accept no arguments and be noexcept");
        if (!detail::table_available(api_, offsetof(FC_NativeApi, expected_fault), sizeof(api_->expected_fault)) || !api_->expected_fault ||
            !resource_) {
            return FaultOutcome::Unavailable;
        }

        // The synchronous ABI call borrows the callable only until completed or recovered execution returns.
        auto thunk = [](void *context) noexcept {
            std::invoke(*static_cast<std::remove_reference_t<F> *>(context));
        };
        return static_cast<FaultOutcome>(api_->expected_fault(resource_, thunk, &callback));
    }

  private:
    void *resource_{};
    const FC_NativeApi *api_{};
};

} // namespace fc
