#pragma once

#include <FusionCutter/patch.hpp>

#include <cstdint>

namespace fusioncutter::patches::direct_transport::server {

// Receives public-endpoint observations without coupling the late observer to the server transport.
class EndpointSink {
  public:
    virtual ~EndpointSink() = default;
    virtual void observe_public_endpoint(std::uint32_t ipv4_network_order) noexcept = 0;
    virtual void observe_unusable_endpoint() noexcept = 0;
};

// Records whether the selected server policy needs the late Galaxy observer.
void request_endpoint_observer(bool requested) noexcept;
[[nodiscard]] bool endpoint_observer_requested() noexcept;
// Publishes the active server transport as the endpoint observation destination.
void publish_endpoint_sink(EndpointSink& sink) noexcept;
void clear_endpoint_sink(EndpointSink& sink) noexcept;
// Forwards a usable or unusable external-address result to the active server transport.
void publish_public_endpoint(std::uint32_t ipv4_network_order) noexcept;
void publish_unusable_endpoint() noexcept;

} // namespace fusioncutter::patches::direct_transport::server
