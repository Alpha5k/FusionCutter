#include "plan.hpp"

#include "plan_detail.hpp"
#include "hook_site.hpp"
#include "reporting/crash_reporter.hpp"

#include <cstring>

namespace fc::runtime {

using plan_detail::take;

namespace {

// Runs a callback on a prepared resource and reports Error or standard exceptions through the C error sink.
// A nonnull handle must refer to a resource whose owning plan is still alive.
template <class Function> std::uint32_t resource_api(void *opaque, FC_ErrorSink sink, Function function) {
    try {
        if (!opaque) {
            throw Error{"Native resource is unavailable"};
        }

        auto &resource = *static_cast<NativeResource *>(opaque);
        if (!resource.ready) {
            throw Error{"Native resource has not been prepared"};
        }

        function(resource);
        return 1;
    } catch (const Error &error) {
        detail::report(sink, error);
    } catch (const std::exception &error) {
        detail::report(sink, Error{error.what()});
    }

    return 0;
}

// Exposes mutable initialization storage only before the owning plan publishes native changes.
std::uint32_t FC_CDECL data_api(void *opaque, void **out, std::uint64_t *count, FC_ErrorSink sink) {
    return resource_api(opaque, sink, [&](NativeResource &resource) {
        if (!out || !count || resource.kind != ResourceKind::Data || resource.plan->exposed()) {
            throw Error{"Data access requires output pointers and unpublished prepared data storage"};
        }

        *out = reinterpret_cast<void *>(resource.address);
        *count = resource.count;
    });
}

// Copies a bounded byte range through the continuing access handle's declared permission.
std::uint32_t FC_CDECL memory_api(void *opaque, std::uint64_t offset, void *value, std::uint64_t size, std::uint32_t write,
                                  FC_ErrorSink sink) {
    return resource_api(opaque, sink, [&](NativeResource &resource) {
        if (resource.kind != ResourceKind::Memory || !value || !size || offset > resource.size || size > resource.size - offset ||
            (write && !resource.writable)) {
            throw Error{"Memory access has an invalid resource, buffer, size, range or write permission"};
        }

        // Page permissions may change after preparation, so check them for each copy.
        const auto address = resource.address + static_cast<std::uintptr_t>(offset);
        take(patching::check_native_access(address, static_cast<std::size_t>(size), write != 0));
        if (write) {
            std::memcpy(reinterpret_cast<void *>(address), value, static_cast<std::size_t>(size));
        } else {
            std::memcpy(value, reinterpret_cast<void *>(address), static_cast<std::size_t>(size));
        }
    });
}

// Returns a borrowed memory view after checking the full range's current permissions.
// The caller keeps the mapped object alive and supplies synchronization during use.
std::uint32_t FC_CDECL view_api(void *opaque, void **out, std::uint64_t *size, FC_ErrorSink sink) {
    return resource_api(opaque, sink, [&](NativeResource &resource) {
        if (!out || !size || resource.kind != ResourceKind::Memory) {
            throw Error{"Memory view requires output pointers and a memory resource"};
        }

        take(patching::check_native_access(resource.address, static_cast<std::size_t>(resource.size), resource.writable));
        *out = reinterpret_cast<void *>(resource.address);
        *size = resource.size;
    });
}

// Supplies a prepared call or allocation address for SDK address resolution.
std::uint32_t FC_CDECL resolve_api(void *opaque, std::uintptr_t *out, FC_ErrorSink sink) {
    return resource_api(opaque, sink, [&](NativeResource &resource) {
        if (!out || (resource.kind != ResourceKind::Call && resource.kind != ResourceKind::Data)) {
            throw Error{"Address resolution requires an output pointer and a call or data resource"};
        }

        *out = resource.address;
    });
}

// Invokes a custom native call using its owned bridge or the shared hook registry's bridge.
std::uint32_t FC_CDECL invoke_api(void *opaque, const FC_NativeFrame *frame, FC_ErrorSink sink) {
    return resource_api(opaque, sink, [&](NativeResource &resource) {
        const auto *invoker = resource.borrowed_invoker ? resource.borrowed_invoker : &resource.invoker;
        if (!frame || resource.kind != ResourceKind::Call || !invoker->address()) {
            throw Error{"Custom native invocation requires a frame, a call resource and a prepared invoker"};
        }

        reinterpret_cast<native::FrameInvoker>(invoker->address())(resource.address, frame);
    });
}

// Runs the callback with crash handling scoped to the declared fault address and access kind.
std::uint32_t FC_CDECL expected_fault_api(void *opaque, FC_ExpectedFaultCallback callback, void *context) {
    const auto *resource = static_cast<NativeResource *>(opaque);
    if (!resource || resource->kind != ResourceKind::Fault || !resource->ready) {
        return 0;
    }

    return CrashReporter::invoke_expected_fault(resource->address, resource->fault_kind, callback, context);
}

} // namespace

const FC_NativeApi *native_api() noexcept {
    static const FC_NativeApi api{sizeof(FC_NativeApi), data_api,   memory_api,         view_api,
                                  resolve_api,          invoke_api, expected_fault_api, hook_detail::current_return_address};
    return &api;
}

} // namespace fc::runtime
