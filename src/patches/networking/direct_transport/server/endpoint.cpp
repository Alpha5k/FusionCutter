#include "endpoint.hpp"

#include <atomic>

namespace fusioncutter::patches::direct_transport::server {
namespace {

std::atomic_bool gObserverRequested{};
PatchInstanceSlot<EndpointSink> gEndpointSink;

} // namespace

void request_endpoint_observer(bool requested) noexcept {
    gObserverRequested.store(requested, std::memory_order_release);
}

bool endpoint_observer_requested() noexcept {
    return gObserverRequested.load(std::memory_order_acquire);
}

void publish_endpoint_sink(EndpointSink& sink) noexcept {
    gEndpointSink.publish(sink);
}

void clear_endpoint_sink(EndpointSink& sink) noexcept {
    gEndpointSink.clear(sink);
}

void publish_public_endpoint(std::uint32_t ipv4_network_order) noexcept {
    if (auto* sink = gEndpointSink.read(); sink != nullptr) {
        sink->observe_public_endpoint(ipv4_network_order);
    }
}

void publish_unusable_endpoint() noexcept {
    if (auto* sink = gEndpointSink.read(); sink != nullptr) {
        sink->observe_unusable_endpoint();
    }
}

} // namespace fusioncutter::patches::direct_transport::server
