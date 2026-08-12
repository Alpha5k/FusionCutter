#include "transport.hpp"

#include <cstdio>
#include <string>

namespace fusioncutter::patches::direct_transport::client {
namespace {

[[nodiscard]] std::uint64_t pack_endpoint(const Endpoint& endpoint) noexcept {
    return endpoint.ipv4_network_order | (static_cast<std::uint64_t>(endpoint.port_host_order) << 32);
}

[[nodiscard]] Endpoint unpack_endpoint(std::uint64_t packed) noexcept {
    return {static_cast<std::uint32_t>(packed), static_cast<std::uint16_t>(packed >> 32)};
}

[[nodiscard]] const char* route_name(RouteState state) noexcept {
    switch (state) {
    case RouteState::Unclassified:
        return "Ready";
    case RouteState::Negotiating:
    case RouteState::AwaitCommit:
        return "Negotiating";
    case RouteState::GalaxyLocked:
        return "Galaxy";
    case RouteState::DirectLocked:
        return "Direct";
    }
    return "Unknown";
}

} // namespace

ClientTransport::ClientTransport(ImageContext image, const GameLayout& layout) noexcept
    : image_(image), layout_(layout) {}

ClientTransport::~ClientTransport() {
    shutdown();
}

std::expected<void, OutcomeReason> ClientTransport::prepare() {
    const auto runtime_data_available = layout_.endpoint_table_rva >= kNativeEndpointConnectedOffset &&
                                        image_.contains_rva(layout_.endpoint_table_rva - kNativeEndpointConnectedOffset,
                                                            kNativeEndpointTableRequiredBytes) &&
                                        image_.contains_rva(layout_.packet_prefix_rva, sizeof(std::uint32_t));
    if (!runtime_data_available) {
        return std::unexpected(OutcomeReason{"Direct Transport client data is outside the recognized image",
                                             "Validate Direct Transport client data",
                                             {}});
    }

    packet_factory_ = {
        image_.function_at_rva<NativePacketAllocate>(layout_.packet_allocate_rva),
        image_.function_at_rva<NativePacketInitialize>(layout_.packet_initialize_rva),
        image_.read_at_rva<std::uint32_t>(layout_.packet_prefix_rva),
    };

    std::int32_t error{};
    const auto available = socket_.start(0, error);
    socket_available_.store(available, std::memory_order_release);
    published_port_.store(available ? socket_.bound_port() : 0, std::memory_order_release);
    if (!available) {
        logging::warning("DirectTransport",
                         "Direct UDP is unavailable; this session will continue over Galaxy (Windows Sockets error " +
                             std::to_string(error) + ")",
                         "Open client UDP socket");
    }
    return {};
}

void ClientTransport::shutdown() noexcept {
    invalidate_association(AssociationEndReason::Shutdown);
    socket_.stop();
    socket_available_.store(false, std::memory_order_release);
    published_port_.store(0, std::memory_order_release);
}

void ClientTransport::write_status(StatusSection& output) const noexcept {
    char socket[48]{};
    if (socket_available_.load(std::memory_order_acquire)) {
        std::snprintf(socket, sizeof(socket), "UDP port %u", published_port_.load(std::memory_order_acquire));
    } else {
        std::snprintf(socket, sizeof(socket), "Unavailable (using Galaxy)");
    }
    output.add("Socket", socket);

    const auto route = published_route_.load(std::memory_order_acquire);
    output.add("Route", route_name(route));
    const auto reason = published_reason_.load(std::memory_order_acquire);
    if (reason != RouteReason::None) {
        output.add("Reason", route_reason_name(reason));
    }
    const auto endpoint = unpack_endpoint(published_endpoint_.load(std::memory_order_acquire));
    if (endpoint.valid()) {
        char server[48]{};
        output.add("Server", format_endpoint(endpoint, server));
    }
    const auto thread_violations = network_thread_.rejected_calls();
    if (thread_violations != 0) {
        char violations[32]{};
        std::snprintf(violations, sizeof(violations), "%llu", static_cast<unsigned long long>(thread_violations));
        output.add("ThreadViolations", violations);
    }
}

GalaxyNetworking ClientTransport::networking() const noexcept {
    using Getter = void*(__cdecl*)();
    const auto getter = image_.function_at_rva<Getter>(layout_.get_networking_rva);
    return getter == nullptr ? GalaxyNetworking{} : GalaxyNetworking::from_interface(getter());
}

bool ClientTransport::read_lobby_owner(std::uint64_t lobby_id, std::uint64_t& owner_user_id) const noexcept {
    constexpr std::size_t kGetLobbyOwnerVtableIndex = 30;
    owner_user_id = 0;
    if (lobby_id == 0) {
        return false;
    }
    using Getter = void*(__cdecl*)();
    const auto getter = image_.function_at_rva<Getter>(layout_.get_matchmaking_rva);
    auto* matchmaking = getter == nullptr ? nullptr : getter();
    if (matchmaking == nullptr) {
        return false;
    }
    auto** vtable = *static_cast<void***>(matchmaking);
    if (vtable == nullptr || vtable[kGetLobbyOwnerVtableIndex] == nullptr) {
        return false;
    }
    using GetLobbyOwner = std::uint64_t*(__thiscall*)(void*, std::uint64_t*, std::uint64_t);
    const auto get_lobby_owner = reinterpret_cast<GetLobbyOwner>(vtable[kGetLobbyOwnerVtableIndex]);
    return get_lobby_owner(matchmaking, &owner_user_id, lobby_id) == &owner_user_id && owner_user_id != 0;
}

bool ClientTransport::read_identity(std::uint8_t physical_primary, std::uint64_t& identity) const noexcept {
    const auto* endpoint_table = reinterpret_cast<const void*>(image_.address_at_rva(layout_.endpoint_table_rva));
    return read_native_identity(endpoint_table, physical_primary, identity);
}

bool ClientTransport::current_host(std::uint64_t identity) const noexcept {
    std::uint64_t current{};
    return read_identity(kHostPrimary, current) && current == identity;
}

void ClientTransport::publish_status() noexcept {
    published_route_.store(association_.live ? association_.state : RouteState::Unclassified,
                           std::memory_order_release);
    published_reason_.store(association_.live ? association_.diagnostics.route_reason() : RouteReason::None,
                            std::memory_order_release);
    published_endpoint_.store(association_.live ? pack_endpoint(association_.server_endpoint) : 0,
                              std::memory_order_release);
}

bool ClientTransport::claim_network_thread(std::string_view operation) noexcept {
    if (network_thread_.claim_current()) {
        return true;
    }
    runtime_log_.error("Direct Transport callback ran outside its network thread", operation);
    return false;
}

bool ClientTransport::on_network_thread(std::string_view operation) noexcept {
    if (network_thread_.is_current()) {
        return true;
    }
    runtime_log_.error("Direct Transport callback ran outside its network thread", operation);
    return false;
}

} // namespace fusioncutter::patches::direct_transport::client
