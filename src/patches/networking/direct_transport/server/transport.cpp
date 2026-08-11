#include "transport.hpp"

#include "layout.hpp"
#include "policy.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

namespace fusioncutter::patches::direct_transport::server {

ServerTransport::ServerTransport(ImageContext image, const GameLayout& game_layout) noexcept
    : image_(image), game_layout_(game_layout) {
    pending_removals_.fill(-1);
}

ServerTransport::~ServerTransport() {
    shutdown();
}

std::expected<void, OutcomeReason> ServerTransport::prepare(Policy policy) {
    policy_ = policy;
    if (policy == Policy::Disabled) {
        return {};
    }
    const auto runtime_data_available =
        game_layout_.endpoint_table_rva >= kNativeEndpointConnectedOffset &&
        image_.contains_rva(game_layout_.endpoint_table_rva - kNativeEndpointConnectedOffset,
                            kNativeEndpointTableRequiredBytes) &&
        image_.contains_rva(game_layout_.packet_prefix_rva, sizeof(std::uint32_t)) &&
        image_.contains_rva(layout::kGamePortRva, sizeof(std::uint32_t));
    if (!runtime_data_available) {
        return std::unexpected(OutcomeReason{"Direct Transport server data is outside the recognized image",
                                             "Validate Direct Transport server data",
                                             {}});
    }

    const auto configured_port = *image_.read_at_rva<std::uint32_t>(layout::kGamePortRva);
    if (configured_port == 0 || configured_port > 0xFFFF) {
        return std::unexpected(
            OutcomeReason{"Direct Transport requires a valid /gameport", "Read server game port", {}});
    }
    game_port_ = static_cast<std::uint16_t>(configured_port);
    packet_factory_ = {
        image_.function_at_rva<NativePacketAllocate>(game_layout_.packet_allocate_rva),
        image_.function_at_rva<NativePacketInitialize>(game_layout_.packet_initialize_rva),
        image_.read_at_rva<std::uint32_t>(game_layout_.packet_prefix_rva),
    };

    std::int32_t error{};
    if (!socket_.start(game_port_, error)) {
        return std::unexpected(OutcomeReason{
            "Direct Transport could not bind its UDP socket: " + std::to_string(error), "Open server UDP socket", {}});
    }
    socket_available_.store(true, std::memory_order_release);
    return {};
}

void ServerTransport::shutdown() noexcept {
    invalidate_all(AssociationEndReason::Shutdown);
    socket_.stop();
    socket_available_.store(false, std::memory_order_release);
    public_ipv4_.store(0, std::memory_order_release);
    endpoint_unavailable_.store(false, std::memory_order_release);
    game_port_ = 0;
}

void ServerTransport::write_status(StatusSection& output) const noexcept {
    static_cast<void>(output.set("Policy", policy_name(policy_)));
    if (policy_ == Policy::Disabled) {
        static_cast<void>(output.set("State", "Disabled"));
        return;
    }

    char socket[48]{};
    if (socket_available_.load(std::memory_order_acquire)) {
        std::snprintf(socket, sizeof(socket), "UDP port %u", game_port_);
    } else {
        std::snprintf(socket, sizeof(socket), "Unavailable");
    }
    static_cast<void>(output.set("Socket", socket));

    const auto public_ipv4 = public_ipv4_.load(std::memory_order_acquire);
    if (public_ipv4 != 0) {
        char endpoint[48]{};
        static_cast<void>(output.set("PublicEndpoint", format_endpoint({public_ipv4, game_port_}, endpoint)));
    } else if (endpoint_unavailable_.load(std::memory_order_acquire)) {
        static_cast<void>(output.set("PublicEndpoint", "Unavailable"));
    } else {
        static_cast<void>(output.set("PublicEndpoint", "Waiting for Galaxy"));
    }

    char routes[80]{};
    std::snprintf(routes, sizeof(routes), "Direct %u, Galaxy %u, Negotiating %u",
                  direct_count_.load(std::memory_order_acquire), galaxy_count_.load(std::memory_order_acquire),
                  negotiating_count_.load(std::memory_order_acquire));
    static_cast<void>(output.set("PlayerRoutes", routes));
    const auto thread_violations = network_thread_.rejected_calls();
    if (thread_violations != 0) {
        char violations[32]{};
        std::snprintf(violations, sizeof(violations), "%llu", static_cast<unsigned long long>(thread_violations));
        static_cast<void>(output.set("ThreadViolations", violations));
    }
}

GalaxyNetworking ServerTransport::networking() const noexcept {
    using Getter = void*(__cdecl*)();
    const auto getter = image_.function_at_rva<Getter>(game_layout_.get_networking_rva);
    return getter == nullptr ? GalaxyNetworking{} : GalaxyNetworking::from_interface(getter());
}

bool ServerTransport::read_identity(std::uint8_t physical_primary, std::uint64_t& identity) const noexcept {
    const auto* endpoint_table = reinterpret_cast<const void*>(image_.address_at_rva(game_layout_.endpoint_table_rva));
    return read_native_identity(endpoint_table, physical_primary, identity);
}

int ServerTransport::resolve_primary(int destination) const noexcept {
    if (destination < 0 || destination >= kNativeEndpointCount) {
        return -1;
    }
    std::uint64_t identity{};
    return read_identity(static_cast<std::uint8_t>(destination), identity) ? find_identity(identity) : -1;
}

int ServerTransport::find_identity(std::uint64_t identity) const noexcept {
    if (identity == 0) {
        return -1;
    }
    for (std::uint8_t physical_primary = 0; physical_primary < associations_.size(); ++physical_primary) {
        std::uint64_t current{};
        if (read_identity(physical_primary, current) && current == identity) {
            return physical_primary;
        }
    }
    return -1;
}

int ServerTransport::find_endpoint(const void* endpoint) const noexcept {
    if (endpoint == nullptr) {
        return -1;
    }
    std::uint64_t identity{};
    std::memcpy(&identity, endpoint, sizeof(identity));
    return find_identity(identity);
}

int ServerTransport::find_connection(std::uint32_t connection_id) const noexcept {
    if (connection_id == 0) {
        return -1;
    }
    const auto found = std::ranges::find_if(associations_, [connection_id](const auto& association) {
        return association.live && association.connection_id == connection_id;
    });
    return found == associations_.end() ? -1 : static_cast<int>(found - associations_.begin());
}

void ServerTransport::observe_public_endpoint(std::uint32_t ipv4_network_order) noexcept {
    if (!is_public_ipv4(ipv4_network_order)) {
        return;
    }
    std::uint32_t expected{};
    public_ipv4_.compare_exchange_strong(expected, ipv4_network_order, std::memory_order_acq_rel,
                                         std::memory_order_acquire);
    endpoint_unavailable_.store(false, std::memory_order_release);
}

void ServerTransport::observe_unusable_endpoint() noexcept {
    public_ipv4_.store(0, std::memory_order_release);
    if (!endpoint_unavailable_.exchange(true, std::memory_order_acq_rel)) {
        runtime_log_.warning("Galaxy reported an unusable public endpoint; Direct Transport is unavailable",
                             "Observe server public endpoint");
    }
}

bool ServerTransport::claim_network_thread(std::string_view operation) noexcept {
    if (network_thread_.claim_current()) {
        return true;
    }
    runtime_log_.error("Direct Transport callback ran outside its network thread", operation);
    return false;
}

bool ServerTransport::on_network_thread(std::string_view operation) noexcept {
    if (network_thread_.is_current()) {
        return true;
    }
    runtime_log_.error("Direct Transport callback ran outside its network thread", operation);
    return false;
}

} // namespace fusioncutter::patches::direct_transport::server
