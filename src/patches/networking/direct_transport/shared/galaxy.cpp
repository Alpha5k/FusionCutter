#include "galaxy.hpp"

#include <limits>

namespace fusioncutter::patches::direct_transport {
namespace {

using SendFunction = bool(__thiscall*)(void*, std::uint64_t, const void*, std::uint32_t, std::uint32_t, std::uint8_t);
using AvailableFunction = bool(__thiscall*)(void*, std::uint32_t*, std::uint8_t);
using ReadFunction = bool(__thiscall*)(void*, void*, std::uint32_t, std::uint32_t*, std::uint64_t*, std::uint8_t);
using PopFunction = void(__thiscall*)(void*, std::uint8_t);

template <typename Function> [[nodiscard]] Function vtable_function(void* context, std::size_t index) noexcept {
    if (context == nullptr) {
        return nullptr;
    }
    const auto vtable = *static_cast<void***>(context);
    return vtable == nullptr ? nullptr : reinterpret_cast<Function>(vtable[index]);
}

[[nodiscard]] bool interface_send(void* context, std::uint64_t peer, const void* bytes, std::uint32_t length,
                                  std::uint32_t send_type, std::uint8_t channel) noexcept {
    const auto function = vtable_function<SendFunction>(context, 1);
    return function != nullptr && function(context, peer, bytes, length, send_type, channel);
}

[[nodiscard]] bool interface_available(void* context, std::uint32_t* bytes, std::uint8_t channel) noexcept {
    const auto function = vtable_function<AvailableFunction>(context, 3);
    return function != nullptr && function(context, bytes, channel);
}

[[nodiscard]] bool interface_read(void* context, void* bytes, std::uint32_t capacity, std::uint32_t* bytes_read,
                                  std::uint64_t* sender, std::uint8_t channel) noexcept {
    const auto function = vtable_function<ReadFunction>(context, 4);
    return function != nullptr && function(context, bytes, capacity, bytes_read, sender, channel);
}

void interface_pop(void* context, std::uint8_t channel) noexcept {
    if (const auto function = vtable_function<PopFunction>(context, 5); function != nullptr) {
        function(context, channel);
    }
}

} // namespace

GalaxyNetworking::GalaxyNetworking(GalaxyApi api) noexcept : api_(api) {}

GalaxyNetworking GalaxyNetworking::from_interface(void* networking) noexcept {
    if (networking == nullptr) {
        return GalaxyNetworking{};
    }
    const auto vtable = *static_cast<void***>(networking);
    if (vtable == nullptr || vtable[1] == nullptr || vtable[3] == nullptr || vtable[4] == nullptr ||
        vtable[5] == nullptr) {
        return GalaxyNetworking{};
    }
    return GalaxyNetworking({networking, &interface_send, &interface_available, &interface_read, &interface_pop});
}

bool GalaxyNetworking::valid() const noexcept {
    return api_.context != nullptr && api_.send != nullptr && api_.available != nullptr && api_.read != nullptr &&
           api_.pop != nullptr;
}

bool GalaxyNetworking::send_reliable_immediate(std::uint64_t peer, std::span<const std::uint8_t> bytes,
                                               std::uint8_t channel) const noexcept {
    return valid() && !bytes.empty() && bytes.size() <= (std::numeric_limits<std::uint32_t>::max)() &&
           api_.send(api_.context, peer, bytes.data(), static_cast<std::uint32_t>(bytes.size()),
                     kGalaxyReliableImmediate, channel);
}

bool GalaxyNetworking::packet_available(std::uint32_t& bytes, std::uint8_t channel) const noexcept {
    bytes = 0;
    if (!valid() || !api_.available(api_.context, &bytes, channel)) {
        bytes = 0;
        return false;
    }
    return true;
}

bool GalaxyNetworking::read_packet(std::span<std::uint8_t> buffer, std::uint32_t& bytes_read, std::uint64_t& sender,
                                   std::uint8_t channel) const noexcept {
    bytes_read = 0;
    sender = 0;
    if (!valid() || buffer.empty() || buffer.size() > (std::numeric_limits<std::uint32_t>::max)() ||
        !api_.read(api_.context, buffer.data(), static_cast<std::uint32_t>(buffer.size()), &bytes_read, &sender,
                   channel) ||
        bytes_read > buffer.size()) {
        bytes_read = 0;
        sender = 0;
        return false;
    }
    return true;
}

bool GalaxyNetworking::pop_packet(std::uint8_t channel) const noexcept {
    if (!valid()) {
        return false;
    }
    api_.pop(api_.context, channel);
    return true;
}

} // namespace fusioncutter::patches::direct_transport
