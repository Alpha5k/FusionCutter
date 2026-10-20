#pragma once

#include <fc/base.hpp>
#include <fc/detail/native_types.hpp>

#include <Winsock2.h>
#include <Windows.h>

#include <cassert>
#include <cstddef>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>
#include <variant>

namespace fc {

// Counted text and byte spans borrow their storage for the lifetime specified by the provider.
using TextView = FC_Text;
using ByteView = FC_Bytes;

// A synchronous C call borrows its bound object; the provider documents thread and buffer lifetimes.
template <class S> struct Function;

template <class R, class... A> struct Function<R(A...) noexcept> {
    // The provider-owned bound object must outlive every call through this table entry.
    void *context{};
    R(FC_CDECL *function)(void *, A...) noexcept {};

    explicit operator bool() const noexcept {
        return function != nullptr;
    }

    // Invoke synchronously on the calling thread; the function pointer must be non-null.
    R operator()(A... arguments) const noexcept {
        assert(function);
        return function(context, std::forward<A>(arguments)...);
    }
};

namespace detail {
template <auto Method, class Signature> struct BoundMethod;

template <auto Method, class R, class C, class... A> struct BoundMethod<Method, R (C::*)(A...) noexcept> {
    // Creates a C table entry borrowing object for every subsequent synchronous member call.
    template <class T> static auto make(T &object) noexcept {
        return Function<R(A...) noexcept>{std::addressof(object), [](void *context, A... arguments) noexcept -> R {
                                              return (static_cast<T *>(context)->*Method)(std::forward<A>(arguments)...);
                                          }};
    }
};

template <auto Method, class R, class C, class... A> struct BoundMethod<Method, R (C::*)(A...) const noexcept> {
    // Creates a borrowed C table entry whose thunk preserves the member function's const access.
    template <class T> static auto make(T &object) noexcept {
        // The ABI erases context constness; the thunk restores it before accessing the object.
        return Function<R(A...) noexcept>{const_cast<std::remove_const_t<T> *>(std::addressof(object)),
                                          [](void *context, A... arguments) noexcept -> R {
                                              return (static_cast<const T *>(context)->*Method)(std::forward<A>(arguments)...);
                                          }};
    }
};
template <auto F, class Signature> struct BoundFunction;

template <auto F, class R, class... A> struct BoundFunction<F, R (*)(A...) noexcept> {
    // Creates a C table entry for F with no bound object.
    static auto make() noexcept {
        return Function<R(A...) noexcept>{nullptr, [](void *, A... arguments) noexcept -> R {
                                              return F(std::forward<A>(arguments)...);
                                          }};
    }
};

} // namespace detail

// Adapts a noexcept member function; object must outlive every use of the returned binding.
template <auto Method, class T> auto bind(T &object) noexcept {
    return detail::BoundMethod<Method, decltype(Method)>::make(object);
}

// Adapts a noexcept free function to the same table entry representation as a bound member.
template <auto F> auto bind() noexcept {
    return detail::BoundFunction<F, decltype(F)>::make();
}

enum class CallbackError : std::uint32_t {
    // The binding or host invocation service is unavailable, or the consumer is not Installed.
    Inactive
};

// Retains stable consumer identity and a borrowed binding; Installed publication makes initialized object state visible.
template <class S> struct ConsumerCallback;

template <class R, class... A> struct ConsumerCallback<R(A...) noexcept> {
    // Stable runtime patch identity remains available even after failed handler startup.
    void *owner{};
    const FC_PatchApi *api{};
    Function<R(A...) noexcept> bound;

    // Returns Inactive before activation or after failed startup; the bound object must survive installed use.
    // Runs synchronously with consumer attribution and restores surrounding Win32/Winsock errors.
    std::expected<R, CallbackError> try_call(A... arguments) const noexcept {
        // Save both thread error values before host checks or author code can change them.
        const auto windows_error = GetLastError();
        const auto socket_error = WSAGetLastError();

        struct Restore {
            DWORD windows_error;
            int socket_error;

            // Restores the calling thread's saved error values on every return path.
            ~Restore() {
                // Restore Win32 last so Winsock restoration cannot overwrite its saved value.
                WSASetLastError(socket_error);
                SetLastError(windows_error);
            }
        } restore{windows_error, socket_error};

        if (!bound || !owner || !detail::table_available(api, offsetof(FC_PatchApi, invoke), sizeof(api->invoke)) || !api->invoke) {
            return std::unexpected(CallbackError::Inactive);
        }

        // Keep arguments and the optional return value in caller storage for the synchronous host invocation.
        struct Invocation {
            const Function<R(A...) noexcept> *bound;
            std::tuple<A...> arguments;
            std::conditional_t<std::is_void_v<R>, std::monostate, std::optional<R>> result;

            // Invokes the typed binding and saves a non-void result in this caller-owned invocation.
            static void FC_CDECL call(void *context) noexcept {
                auto &value = *static_cast<Invocation *>(context);
                if constexpr (std::is_void_v<R>) {
                    std::apply(*value.bound, value.arguments);
                } else {
                    value.result.emplace(std::apply(*value.bound, value.arguments));
                }
            }
        } invocation{&bound, {std::forward<A>(arguments)...}, {}};

        // The runtime checks activity before invoking this local thunk and contains live native faults.
        if (api->invoke(owner, &Invocation::call, &invocation) != 1) {
            return std::unexpected(CallbackError::Inactive);
        }
        if constexpr (std::is_void_v<R>) {
            return {};
        } else {
            return std::move(*invocation.result);
        }
    }
};

// Stores a record's layout description returned by fc_description(std::type_identity<T>) through ADL.
struct RecordDescription {
    // Encoded ID, byte size, alignment, and each field's offset and type description.
    std::vector<std::uint8_t> bytes;
};

namespace detail {
template <class T> inline thread_local std::string_view active_record_id;

// Tracks the record ID while recursively describing its fields, then restores the enclosing description.
template <class T> struct RecordDescriptionScope {
    std::string_view previous{active_record_id<T>};

    // Makes this borrowed ID available to recursive pointers until the scope exits.
    explicit RecordDescriptionScope(std::string_view id) {
        active_record_id<T> = id;
    }

    // Restores the prior ID on completion or exception unwinding.
    ~RecordDescriptionScope() {
        active_record_id<T> = previous;
    }
};

// Appends a 32-bit number to the contract description.
inline void description_number(std::vector<std::uint8_t> &out, std::uint32_t value) {
    // Encode fixed-width numbers least-significant byte first for reproducible contract comparison.
    for (unsigned i = 0; i != 4; ++i) {
        out.push_back(static_cast<std::uint8_t>(value >> (i * 8)));
    }
}

// Appends counted identifier bytes; throws when the byte count exceeds the ABI range.
inline void description_bytes(std::vector<std::uint8_t> &out, std::string_view value) {
    // Prefix identifiers with their byte count so adjacent descriptions remain unambiguous.
    description_number(out, count(value.size()));
    out.insert(out.end(), value.begin(), value.end());
}
template <class T> struct ApiShape;

// Appends T's ABI representation; author records provide fc_description through argument-dependent lookup.
template <class T> void describe_type(std::vector<std::uint8_t> &out) {
    using U = std::remove_cv_t<T>;
    static_assert(!std::is_reference_v<T>, "Patch API types cannot be references; use values or borrowed pointers");
    // Type tags distinguish equal-sized values whose calling or data representations differ.
    if constexpr (std::is_void_v<U>) {
        out.push_back(0);
    } else {
        static_assert(std::is_trivially_copyable_v<U> && std::is_standard_layout_v<U>);
        description_number(out, count(sizeof(U)));
        description_number(out, count(alignof(U)));
        if constexpr (std::is_array_v<U>) {
            out.push_back(11);
            detail::description_number(out, detail::count(std::extent_v<U>));
            describe_type<std::remove_extent_t<U>>(out);
        } else if constexpr (std::is_integral_v<U>) {
            static_assert(!std::is_same_v<U, bool>, "Patch APIs cannot use bool; use a fixed-width integer with values 0 or 1");
            out.push_back(std::is_signed_v<U> ? 1 : 2);
        } else if constexpr (std::is_enum_v<U>) {
            out.push_back(3);
            describe_type<std::underlying_type_t<U>>(out);
        } else if constexpr (std::is_floating_point_v<U>) {
            static_assert(std::is_same_v<U, float> || std::is_same_v<U, double>);
            out.push_back(4);
        } else if constexpr (std::is_pointer_v<U>) {
            out.push_back(5);
            out.push_back(std::is_const_v<std::remove_pointer_t<U>> ? 1 : 0);
            describe_type<std::remove_pointer_t<U>>(out);
        } else if constexpr (requires { ApiShape<U>::append(out); }) {
            // SDK function, callback and counted-span types have fixed descriptions rather than author records.
            ApiShape<U>::append(out);
        } else {
            if (!active_record_id<U>.empty()) {
                // Recursive pointers reference the active record ID after the already-encoded size and alignment.
                out.push_back(12);
                description_bytes(out, active_record_id<U>);
            } else {
                // Ask the record's namespace to describe its declared fields and embed those bytes.
                out.push_back(6);
                const auto record = fc_description(std::type_identity<U>{});
                description_number(out, count(record.bytes.size()));
                out.insert(out.end(), record.bytes.begin(), record.bytes.end());
            }
        }
    }
}

template <class R, class... A> struct ApiShape<Function<R(A...) noexcept>> {
    // Appends a function tag, return type and ordered argument types.
    static void append(std::vector<std::uint8_t> &out) {
        out.push_back(7);
        describe_type<R>(out);
        description_number(out, sizeof...(A));
        (describe_type<A>(out), ...);
    }
};

template <class R, class... A> struct ApiShape<ConsumerCallback<R(A...) noexcept>> {
    // Distinguishes a retained consumer callback from an ordinary function with the same signature.
    static void append(std::vector<std::uint8_t> &out) {
        out.push_back(8);
        ApiShape<Function<R(A...) noexcept>>::append(out);
    }
};

template <> struct ApiShape<FC_Text> {
    // Appends the text-view tag and its pointer and count offsets.
    static void append(std::vector<std::uint8_t> &out) {
        out.push_back(9);
        description_number(out, offsetof(FC_Text, data));
        description_number(out, offsetof(FC_Text, size));
    }
};

template <> struct ApiShape<FC_Bytes> {
    // Appends the byte-view tag and its pointer and count offsets.
    static void append(std::vector<std::uint8_t> &out) {
        out.push_back(10);
        description_number(out, offsetof(FC_Bytes, data));
        description_number(out, offsetof(FC_Bytes, size));
    }
};

// Returns a member's byte offset; T must permit value initialization and the result must fit the ABI count.
template <class T, class M> std::uint32_t member_offset(M T::*member) {
    // Measure the field's byte offset in a real object, including padding chosen by the compiler.
    T value{};
    return count(reinterpret_cast<const std::byte *>(std::addressof(value.*member)) -
                 reinterpret_cast<const std::byte *>(std::addressof(value)));
}

// Appends the member's physical offset followed by its ABI type description.
template <class T, class M> void describe_member(std::vector<std::uint8_t> &out, M T::*member) {
    description_number(out, member_offset(member));
    describe_type<M>(out);
}

template <class T> struct IsApiFunction : std::false_type {};

template <class S> struct IsApiFunction<Function<S>> : std::true_type {};
} // namespace detail

// Builds a record description; list every field once in declaration order under a nonempty stable ID.
// Define fc_description in the record's namespace for nested records; pointer cycles reference their active record ID.
template <class T, class... M> RecordDescription describe(std::string_view id, M T::*...members) {
    static_assert(std::is_standard_layout_v<T> && std::is_trivially_copyable_v<T>);
    if (id.empty()) {
        throw std::invalid_argument("Patch API record type ID is empty");
    }

    detail::RecordDescriptionScope<T> scope{id};
    // Encode the record identity and layout before recursively describing members in the supplied order.
    RecordDescription result;
    detail::description_bytes(result.bytes, id);
    detail::description_number(result.bytes, detail::count(sizeof(T)));
    detail::description_number(result.bytes, detail::count(alignof(T)));
    detail::description_number(result.bytes, sizeof...(M));
    (detail::describe_member(result.bytes, members), ...);
    return result;
}

// SDK storage owns contract bytes; every C view borrows them for one synchronous Prepare operation.
template <class T> class ApiContract {
  public:
    ApiContract(std::string id, std::vector<std::uint8_t> description, std::vector<std::uint32_t> functions)
        : id_(std::move(id)), description_(std::move(description)), functions_(std::move(functions)) {}

    // Returns a counted ABI view whose referenced storage remains owned by this contract.
    FC_ApiContract view() const {
        return {detail::text(id_),         detail::count(sizeof(T)),
                detail::count(alignof(T)), {description_.data(), detail::count(description_.size())},
                functions_.data(),         detail::count(functions_.size())};
    }

  private:
    std::string id_;
    std::vector<std::uint8_t> description_;
    // Byte offsets of required function pointers, excluding the context stored beside each pointer.
    std::vector<std::uint32_t> functions_;
};

// Builds an API contract from every Function member listed once in declaration order under a nonempty ID.
// Use a new versioned API ID when behavior changes; nested records need fc_description in their namespace.
template <class T, class... M> ApiContract<T> api(std::string_view id, M T::*...members) {
    static_assert(std::is_standard_layout_v<T> && std::is_trivially_copyable_v<T>);
    static_assert((detail::IsApiFunction<M>::value && ...), "Listed patch API table members must have type fc::Function");
    auto description = describe<T>(id, members...);
    // Locate each callable pointer inside its Function member so the runtime can reject null entries.
    std::vector<std::uint32_t> functions;
    (functions.push_back(detail::member_offset(members) + detail::count(offsetof(M, function))), ...);
    return ApiContract<T>{std::string{id}, std::move(description.bytes), std::move(functions)};
}

namespace detail {
// Returns the patch API service when its field is present in the host's published ABI prefix.
inline const FC_PatchApi *patch_service(const FC_Host *host) noexcept {
    // Size checks allow SDK code to detect hosts whose ABI table predates this service.
    return table_available(host, offsetof(FC_Host, patch_api), sizeof(host->patch_api)) ? host->patch_api : nullptr;
}

// Publishes a typed table through the owner's Prepare service, returning a captured host error on failure.
template <class T> Result<> publish_api(void *owner, const FC_Host *host, const ApiContract<T> &contract, const T &table) {
    auto *service = patch_service(host);
    if (!table_available(service, offsetof(FC_PatchApi, publish), sizeof(service->publish)) || !service->publish) {
        return std::unexpected(Error{"Patch API publication is unavailable"});
    }

    NativeError error;
    const auto description = contract.view();
    // The host copies contract and table bytes before this synchronous call returns.
    if (service->publish(owner, &description, &table, error.sink()) != 1) {
        return std::unexpected(error.failure());
    }
    return {};
}

// Retrieves a compatible declared provider table through the owner's Prepare service into a value copy.
template <class T> Result<T> get_api(void *owner, const FC_Host *host, std::string_view provider, const ApiContract<T> &contract) {
    auto *service = patch_service(host);
    if (!table_available(service, offsetof(FC_PatchApi, get), sizeof(service->get)) || !service->get) {
        return std::unexpected(Error{"Required patch API retrieval is unavailable"});
    }

    NativeError error;
    const auto description = contract.view();
    T result{};
    // Fill local typed storage only after the host verifies the provider's contract and required functions.
    if (service->get(owner, text(provider), &description, &result, error.sink()) != 1) {
        return std::unexpected(error.failure());
    }
    return result;
}

// Keeps the author's notification callable in the handler's DLL until handler storage is destroyed.
template <class T, class F> struct AvailableCapture final : CallbackCapture {
    explicit AvailableCapture(F value) : function(std::move(value)) {}

    F function;

    // Delivers the compatible table as T to the retained noexcept notification callable.
    static void FC_CDECL invoke(void *context, const void *table) noexcept {
        auto &capture = *static_cast<AvailableCapture *>(context);
        // The aligned runtime table remains valid after this notification; the callable is owned by this DLL.
        std::invoke(capture.function, *static_cast<const T *>(table));
    }
};

// Registers one optional notification during Prepare; captures must outlive delivery or discarded preparation.
// The callback runs synchronously on the control thread and must accept a const T reference without throwing.
template <class T, class F>
Result<> on_api_available(void *owner, const FC_Host *host, CallbackCaptures *captures, std::string_view provider,
                          const ApiContract<T> &contract, F &&function) {
    using Capture = AvailableCapture<T, std::decay_t<F>>;
    static_assert(std::is_nothrow_invocable_v<std::decay_t<F> &, const T &>,
                  "API notifications must accept a const table reference without throwing");
    auto *service = patch_service(host);
    if (!captures || !table_available(service, offsetof(FC_PatchApi, on_available), sizeof(service->on_available)) ||
        !service->on_available) {
        return std::unexpected(Error{"Optional patch API notification is unavailable"});
    }

    // Retain the callable before registering its stable address with the runtime.
    auto capture = std::make_unique<Capture>(std::forward<F>(function));
    auto *context = capture.get();
    captures->push_back(std::move(capture));

    NativeError error;
    const auto description = contract.view();
    if (service->on_available(owner, text(provider), &description, context, &Capture::invoke, error.sink()) != 1) {
        // Failed registration leaves no subscription that needs this callable's storage.
        captures->pop_back();
        return std::unexpected(error.failure());
    }
    return {};
}

// Builds a retained binding with stable runtime identity; the bound object must survive installed callback use.
template <class S> ConsumerCallback<S> consumer_callback(void *owner, const FC_Host *host, Function<S> function) noexcept {
    return {owner, patch_service(host), function};
}
} // namespace detail
} // namespace fc
