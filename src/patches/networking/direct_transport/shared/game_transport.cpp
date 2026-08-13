#include "game_transport.hpp"

namespace fusioncutter::patches::direct_transport {
namespace {

[[nodiscard]] GameTransport& transport_from(void* context) noexcept {
    return *static_cast<GameTransport*>(context);
}

void before_receive(void* context) noexcept {
    transport_from(context).before_receive();
}

void after_receive(void* context) noexcept {
    transport_from(context).after_receive();
}

void native_transmit(void* context, int destination) noexcept {
    transport_from(context).on_native_transmit(destination);
}

int begin_group(void* context, int destination) noexcept {
    return transport_from(context).begin_transmit_group(destination);
}

void end_group(void* context, int destination) noexcept {
    transport_from(context).end_transmit_group(destination);
}

network_pipeline::NativeSendResult send(void* context, int destination, int group,
                                        std::span<const std::uint8_t> bytes) noexcept {
    const auto result = transport_from(context).transmit_native(destination, group, bytes);
    return {
        .result = result.result,
        .error = result.error,
        .handled = result.handled,
        .carrier = result.handled && result.result >= 0 ? network_pipeline::PacketCarrier::Direct
                                                        : network_pipeline::PacketCarrier::NotSent,
    };
}

void intake(void* context, void* endpoint) noexcept {
    transport_from(context).on_native_intake(endpoint);
}

void disconnect(void* context, int destination) noexcept {
    transport_from(context).on_native_disconnect(destination);
}

void disconnect_complete(void* context, int destination) noexcept {
    transport_from(context).on_native_disconnect_complete(destination);
}

void reset(void* context, std::uint8_t mode) noexcept {
    transport_from(context).on_reset(mode);
}

} // namespace

network_pipeline::TransportCallbacks make_pipeline_callbacks(GameTransport& transport) noexcept {
    return {
        .context = &transport,
        .before_receive = &before_receive,
        .after_receive = &after_receive,
        .native_transmit = &native_transmit,
        .begin_group = &begin_group,
        .end_group = &end_group,
        .send = &send,
        .intake = &intake,
        .disconnect = &disconnect,
        .disconnect_complete = &disconnect_complete,
        .reset = &reset,
    };
}

} // namespace fusioncutter::patches::direct_transport
