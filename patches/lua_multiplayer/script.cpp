#include "service.hpp"

#include "protocol.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>

namespace fc::core_plugin::lua_multiplayer {
namespace {

constexpr std::string_view kPeerTag = "FusionCutter.Multiplayer.PeerId";

// Checks channel names for sends and readiness queries.
std::optional<std::string_view> channel_argument(lua::Frame &frame, std::size_t index) {
    auto channel = frame.argument(index).as_string();
    if (!channel || !protocol::valid_channel(*channel)) {
        frame.error("Expected a nonempty UTF-8 channel name");
        return std::nullopt;
    }
    return *channel;
}

// Listen and Close report ordinary registration failures as nil and a message.
void return_result(lua::Frame &frame, fc::Result<> result) {
    if (result) {
        frame.return_values({frame.value(true)});
    } else {
        frame.return_values({frame.nil(), frame.value(result.error().message)});
    }
}

// Returns nil and a message when a script query has no Game context.
void unavailable_context(lua::Frame &frame) {
    frame.return_values({frame.nil(), frame.value("Game context is unavailable")});
}

} // namespace

void Service::initialize_peer_cache(lua::Frame &frame) {
    // The cache holds weak references: Lua can collect peer objects once scripts release them.
    auto result = frame.execute(R"(
        local cache = setmetatable({}, {__mode = "v"})
        return function(key, value)
            local found = cache[key]
            if found ~= nil then return found end
            if value ~= nil then cache[key] = value end
            return value
        end
    )");
    if (!result) {
        frame.error(result.error().message);
        return;
    }

    auto function = frame.retain((*result)[0]);
    if (!function) {
        frame.error(function.error().message);
        return;
    }
    set_peer_cache(frame.state(), std::make_shared<lua::FunctionRef>(std::move(*function)));
}

lua::Value Service::peer_value(lua::Frame &frame, PeerId peer) {
    if (!peer.value) {
        return frame.nil();
    }

    // Keep a shared reference to the cache function while using Lua outside the service mutex.
    auto cache = peer_cache(frame.state());
    if (!cache) {
        frame.error("Multiplayer peer cache is unavailable");
        return frame.nil();
    }

    // Use all eight ID bytes as a string key to preserve the full identity in Lua's cache.
    std::array<std::byte, sizeof(peer.value)> bytes;
    std::memcpy(bytes.data(), &peer.value, bytes.size());
    const auto key = frame.value(std::string_view{reinterpret_cast<const char *>(bytes.data()), bytes.size()});
    auto found = frame.call(*cache, {key});
    if (!found) {
        frame.error(found.error().message);
        return frame.nil();
    }
    if ((*found)[0].type() != lua::Type::Nil) {
        return (*found)[0];
    }

    const auto value = frame.userdata(kPeerTag, bytes);
    auto inserted = frame.call(*cache, {key, value});
    if (!inserted) {
        frame.error(inserted.error().message);
        return frame.nil();
    }
    return (*inserted)[0];
}

std::optional<PeerId> Service::read_peer(lua::Value value) {
    const auto bytes = value.as_userdata(kPeerTag);
    if (!bytes || bytes->size() != sizeof(std::uint64_t)) {
        return std::nullopt;
    }

    PeerId peer;
    std::memcpy(&peer.value, bytes->data(), sizeof(peer.value));
    return peer.value ? std::optional{peer} : std::nullopt;
}

fc::Result<> Service::register_script(lua::Bindings &bindings) {
    if (auto result = bindings.initialize([this](lua::Frame &frame) {
            initialize_peer_cache(frame);
        });
        !result) {
        return result;
    }

    // Register each function under FusionCutter.Multiplayer, stopping after the first registration error.
    fc::Result<> result;
    const auto bind = [&]<class F>(std::string_view name, F function) {
        if (result) {
            result = bindings.bind({"FusionCutter", "Multiplayer", name}, std::move(function));
        }
    };

    bind("Listen", [this](lua::Frame &frame) {
        const auto channel = frame.argument(0).as_string();
        if (!channel) {
            return_result(frame, std::unexpected(Error{"Expected a nonempty UTF-8 channel name"}));
            return;
        }
        const auto handler = frame.argument(1);
        return_result(frame, listen(frame, *channel, handler));
    });

    bind("Close", [this](lua::Frame &frame) {
        if (const auto channel = frame.argument(0).as_string()) {
            return_result(frame, close(frame.state(), *channel));
        } else {
            return_result(frame, std::unexpected(Error{"Expected a nonempty UTF-8 channel name"}));
        }
    });

    bind("SendHost", [this](lua::Frame &frame) {
        if (const auto channel = channel_argument(frame, 0)) {
            frame.return_values({frame.value(send_host(*channel, frame.argument(1)))});
        }
    });

    bind("SendClient", [this](lua::Frame &frame) {
        const auto peer = read_peer(frame.argument(0));
        if (!peer) {
            frame.error("Expected a multiplayer peer identity");
            return;
        }

        if (const auto channel = channel_argument(frame, 1)) {
            frame.return_values({frame.value(send_client(*peer, *channel, frame.argument(2)))});
        }
    });

    bind("Broadcast", [this](lua::Frame &frame) {
        if (const auto channel = channel_argument(frame, 0)) {
            frame.return_values({frame.value(broadcast(*channel, frame.argument(1)))});
        }
    });

    bind("Host", [this](lua::Frame &frame) {
        const auto view = snapshot();
        if (!view.state) {
            unavailable_context(frame);
            return;
        }

        frame.return_values({peer_value(frame, view.host)});
    });

    bind("Clients", [this](lua::Frame &frame) {
        const auto view = snapshot();
        if (!view.state) {
            unavailable_context(frame);
            return;
        }

        auto clients = frame.table();
        // Lua array indices start at one.
        for (std::size_t i = 0; i < view.clients.size(); ++i) {
            clients.set(i + 1, peer_value(frame, view.clients[i]));
        }
        frame.return_values({clients});
    });

    bind("IsHost", [this](lua::Frame &frame) {
        const auto view = snapshot();
        if (!view.state) {
            unavailable_context(frame);
            return;
        }

        frame.return_values({frame.value(view.state->is_host())});
    });

    bind("IsClient", [this](lua::Frame &frame) {
        const auto view = snapshot();
        if (!view.state) {
            unavailable_context(frame);
            return;
        }

        frame.return_values({frame.value(view.state->is_client())});
    });

    bind("Ready", [this](lua::Frame &frame) {
        const auto peer = read_peer(frame.argument(0));
        if (!peer) {
            frame.error("Expected a multiplayer peer identity");
            return;
        }

        if (const auto channel = channel_argument(frame, 1)) {
            frame.return_values({frame.value(ready(*peer, *channel))});
        }
    });

    return result;
}

} // namespace fc::core_plugin::lua_multiplayer
